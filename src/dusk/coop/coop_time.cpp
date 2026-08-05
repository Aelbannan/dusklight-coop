#include "dusk/coop/coop_time.h"

#include "dusk/coop/coop.h"

#include "SSystem/SComponent/c_counter.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "d/d_kankyo_wether.h"
#include "d/d_msg_object.h"
#include "d/d_save.h"
#include "dusk/net/clock.h"
#include "f_op/f_op_msg.h"
#include "m_Do/m_Do_audio.h"

#include <aurora/lib/logging.hpp>

#include <chrono>
#include <cstring>

namespace dusk::coop::timeweather {

namespace {

aurora::Module TimeLog("dusk::coop::timeweather");

using net::kMaxStageNameLength;
using net::kTimeFlagDarkworld;
using net::kTimeRateFast;
using net::kTimeRateFrozen;
using net::kTimeRateNormal;
using net::kTimeRatePond2x;
using net::MsgType;
using net::PayloadUnion;
using net::TimeEventId;
using net::TimeStateInfo;
using net::WeatherMode;
using net::WeatherStateInfo;

// ---------------------------------------------------------------------------
// Replica targets (client): the host's clock/sky as last known. Inert until
// a session is live and a target was received/seeded.
// ---------------------------------------------------------------------------

struct SyncTime : TimeStateInfo {
    bool valid = false;
};
struct SyncWeather : WeatherStateInfo {
    bool valid = false;
};

SyncTime g_time;
SyncWeather g_weather;
bool g_clientSeeded = false;
// Last world state seeded FROM (session worldTime/weather). Kept separate
// from g_time/g_weather so a stale JoinAccept/WorldInit copy can never
// regress fresher TimeSync/WeatherChange receipts: re-seeding triggers only
// when the session's world state itself changes (a join or roster-refresh).
TimeStateInfo g_worldSeenTime{};
WeatherStateInfo g_worldSeenWeather{};

// ---------------------------------------------------------------------------
// Host publisher state
// ---------------------------------------------------------------------------

net::NetClock g_syncClock;  // 1 Hz TimeSync cadence (wall clock)
char g_lastStage[kMaxStageNameLength] = {};
bool g_stageSeeded = false;
f32 g_lastPhase = 0.0f;  // observed phase (end of last sim tick)
u16 g_lastDay = 0;
u8 g_lastRate = 0xFF;   // last published rate bucket
u8 g_lastMode = 0xFF;   // last published derived WeatherMode
u16 g_lastIntensity = 0;  // last published intensity (SNOW drift refresh)

// ---------------------------------------------------------------------------
// Guards
// ---------------------------------------------------------------------------

/// A client with an accepted session. The host never runs the replica/force —
/// it sims the clock and sky natively.
bool ClientActive() {
    return dusk::coop::sessionActive() && !dusk::coop::hostRole();
}

u64 NowUs() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// ---------------------------------------------------------------------------
// Rate model (host side; 04 §1.2/§5.3/§5.4/§5.11/§5.12)
// ---------------------------------------------------------------------------

/// Mirrors the vanilla advance gate in setDaytime() exactly; rate=0 while any
/// freeze condition holds (04 §5.3 — a client's own event/message box never
/// freezes the world clock because the rate is host-derived).
bool ClockFrozen() {
    dScnKy_env_light_c& env = g_env_light;
    if (env.using_time_control_tag != 0 || env.field_0x130a != 0) {
        return true;
    }
    if (dComIfGp_event_runCheck() != FALSE) {
        return true;
    }
    msg_class* msg = dMsgObject_c::getActor();
    if (msg != nullptr && msg->mode >= 2) {
        return true;
    }
    if (dComIfGp_roomControl_getTimePass() == FALSE) {
        return true;
    }
    return false;
}

u8 HostRate() {
    if (ClockFrozen()) {
        return kTimeRateFrozen;
    }
    if (dKy_darkworld_check()) {
        // The twilight branch replaces the day clock (daytime pinned 0, 04
        // §5.5); the synced rate is meaningless there — clients in twilight
        // run their own darkworld clock. flags carries the darkworld bit.
        return kTimeRateFrozen;
    }
    const f32 rate = g_env_light.time_change_rate;
    if (rate >= 1000.0f) {
        // DEBUG / save-menu time-pick markers (04 §5.12): the skip is
        // instantaneous on the host; clients hold until the next absolute
        // TimeSync (immediate on rate change) rather than chasing a
        // 1000+/tick phase.
        return kTimeRateFrozen;
    }
    if (rate == 1.0f) {
        return kTimeRateFast;  // wolf-howl fast-forward (04 §5.4)
    }
    const char* stage = dComIfGp_getStartStageName();
    if (stage != nullptr &&
        (std::strcmp(stage, "F_SP127") == 0 || std::strcmp(stage, "R_SP127") == 0))
    {
        return kTimeRatePond2x;  // Fishing Pond / Hena's Hut (04 §5.11)
    }
    return kTimeRateNormal;
}

// ---------------------------------------------------------------------------
// Host weather derivation (04 §4.3). Two sources, both authoritative for
// their stages:
//  - dice stages (F_SP108/121/127, kytag06 type 4): the dice machine's own
//    mode — exact mode-change timing including the Cloudy/RainLight teardown
//    distinction that raw state cannot see (both share colpat 1).
//  - everywhere else: the LIVE sky state — kytag06 boss-arena weather, the
//    stage-file snow drive — so a mode change is published exactly when the
//    sky actually changes. (Boss-arena weather is thus carried by the same
//    channel; see the M3 summary for the note vs 04 §5.7 "stays local".)
// A drain is recognizable even without the dice: colpat 0 with rain in the
// air is always a teardown in the engine (no mode pairs colpat 0 with rain),
// so it derives to Clear with the carried intensity — the client pins it and
// drains with the same dice_rain_minus rule.
// ---------------------------------------------------------------------------

u16 ClampU16(int v) {
    return v > 0xFFFF ? 0xFFFF : static_cast<u16>(v);
}

bool IsDiceStage(const char* stage) {
    return stage != nullptr && (std::strcmp(stage, "F_SP108") == 0 ||
                                std::strcmp(stage, "F_SP121") == 0 ||
                                std::strcmp(stage, "F_SP127") == 0);
}

void DeriveWeather(u8& outMode, u8& outThunder, u16& outIntensity, u8& outColpat) {
    const dScnKy_env_light_c& env = g_env_light;
    outThunder = env.mThunderEff.mMode != 0 ? 1 : 0;
    outColpat = env.mColpatWeather;
    // Snow first: mSnowCount > 0 only in snow stages, where the dice machine
    // never runs (its mode would be a stale overworld leftover).
    if (env.mSnowCount > 0) {
        outMode = static_cast<u8>(WeatherMode::Snow);
        outIntensity = ClampU16(env.mSnowCount);
        return;
    }
    const char* stage = dComIfGp_getStartStageName();
    if (IsDiceStage(stage) && env.dice_wether_mode <= DICE_MODE_THUNDER_HEAVY_e) {
        // Direct dice-mode mapping (04 §4.3 table): Sunny->Clear, Cloudy->
        // Cloudy, RainLight->RainLight, RainHeavy->RainHeavy,
        // ThunderLight/Heavy->ThunderLight/Heavy, UNK6->Clear.
        static const u8 kDiceModeToWeather[7] = {
            static_cast<u8>(WeatherMode::Clear),
            static_cast<u8>(WeatherMode::Cloudy),
            static_cast<u8>(WeatherMode::RainLight),
            static_cast<u8>(WeatherMode::RainHeavy),
            static_cast<u8>(WeatherMode::ThunderLight),
            static_cast<u8>(WeatherMode::ThunderHeavy),
            static_cast<u8>(WeatherMode::Clear),
        };
        outMode = kDiceModeToWeather[env.dice_wether_mode];
        outIntensity = ClampU16(env.raincnt);
        return;
    }
    if (env.raincnt > 0) {
        if (outThunder != 0) {
            outMode = static_cast<u8>(outColpat >= 2 ? WeatherMode::ThunderHeavy
                                                     : WeatherMode::ThunderLight);
        } else if (outColpat >= 2) {
            outMode = static_cast<u8>(WeatherMode::RainHeavy);
        } else if (outColpat >= 1) {
            outMode = static_cast<u8>(WeatherMode::RainLight);
        } else {
            outMode = static_cast<u8>(WeatherMode::Clear);  // colpat 0 + rain = drain
        }
        outIntensity = ClampU16(env.raincnt);
        return;
    }
    // No rain, no snow: clear or cloudy from the palette (thunder only ever
    // coexists with colpat >= 1 in the engine, so a stray thunder-without-
    // rain collapses into Cloudy/Clear — same as the dice proc's gate).
    outMode = static_cast<u8>(outColpat >= 1 ? WeatherMode::Cloudy : WeatherMode::Clear);
    outIntensity = 0;
}

// ---------------------------------------------------------------------------
// Host publisher
// ---------------------------------------------------------------------------

void SendMessage(MsgType type, const PayloadUnion& payload) {
    if (!dusk::coop::sendGameMessage(type, payload)) {
        TimeLog.debug("m3: dropped {} (session not sendable)", static_cast<u16>(type));
    }
}

void SendTimeSync(f32 time, u16 day, u8 rate, u8 flags) {
    PayloadUnion p = {};
    p.timeSync.time = time;
    p.timeSync.day = day;
    p.timeSync.rate = rate;
    p.timeSync.flags = flags;
    SendMessage(MsgType::TimeSync, p);
}

void SendTimeEvent(TimeEventId id, f32 time, u16 day) {
    PayloadUnion p = {};
    p.timeEvent.eventId = static_cast<u8>(id);
    p.timeEvent.time = time;
    p.timeEvent.day = day;
    SendMessage(MsgType::TimeEvent, p);
}

void SendWeatherChange(u8 mode, u8 thunder, u16 intensity, u8 colpat) {
    PayloadUnion p = {};
    p.weatherChange.mode = mode;
    p.weatherChange.thunder = thunder;
    p.weatherChange.intensity = intensity;
    p.weatherChange.colpat = colpat;
    SendMessage(MsgType::WeatherChange, p);
}

/// Host tick: observes the frame-boundary clock/sky state and publishes.
/// Runs pre-actor-phase (before exeKankyo's advance), so an observation is
/// the END of the previous frame's setDaytime — one tick late, invisible on
/// reliable events and self-correcting on the 1 Hz absolute TimeSync.
void PublishHostState() {
    const f32 now = dComIfGs_getTime();
    const u16 day = dComIfGs_getDate();
    const u8 rate = HostRate();
    const u8 flags = dKy_darkworld_check() ? kTimeFlagDarkworld : 0;

    const bool oneSecondDue = g_syncClock.Tick(NowUs());

    const char* stage = dComIfGp_getStartStageName();
    const char* stageName = (stage != nullptr && stage[0] != '\0') ? stage : "";
    const bool stageChanged = std::strcmp(stageName, g_lastStage) != 0;
    if (stageChanged) {
        std::strncpy(g_lastStage, stageName, kMaxStageNameLength - 1);
        g_lastStage[kMaxStageNameLength - 1] = '\0';
        // Stage transitions RESET time (stagInfo hour / nexttime, 04 §5.1)
        // and fully reset weather (04 §5.6): re-seed the trackers; the
        // immediate sends below let clients re-assert promptly (04 §5.10).
        g_lastPhase = now;
        g_lastDay = day;
        g_lastMode = 0xFF;
        g_stageSeeded = false;
    }

    // Boundary events (advisory, reliable). A day rollover wins; otherwise
    // detect upward crossings of 90 (dawn) / 285 (dusk) between consecutive
    // observations.
    if (!g_stageSeeded) {
        g_stageSeeded = true;
        g_lastPhase = now;
        g_lastDay = day;
        // First observation seeds the 1 Hz clock cadence too.
        g_syncClock.Reset(NowUs());
    } else if (!stageChanged) {
        if (day != g_lastDay) {
            SendTimeEvent(TimeEventId::NewDay, now, day);
        } else {
            if (g_lastPhase < 90.0f && now >= 90.0f) {
                SendTimeEvent(TimeEventId::Dawn, now, day);
            } else if (g_lastPhase < 285.0f && now >= 285.0f) {
                SendTimeEvent(TimeEventId::Dusk, now, day);
            }
        }
        g_lastPhase = now;
        g_lastDay = day;
    }

    // 1 Hz absolute TimeSync; immediate on stage change and on rate
    // transitions (04 §4.1 — clients stop fast-forwarding / resume promptly
    // instead of up to 1 s late).
    if (oneSecondDue || stageChanged || rate != g_lastRate) {
        SendTimeSync(now, day, rate, flags);
    }
    g_lastRate = rate;

    // Weather: publish on mode change + after stage change. While it stays
    // SNOW, also re-publish on meaningful intensity drift — the client pins
    // mSnowCount to the carried intensity (no per-mode ramp for snow, and its
    // local kytag00 area tags move it slowly), so a 1 Hz-ish refresh keeps
    // Snowpeak's density in step without streaming the rain ramp (04 §4.3).
    u8 mode = 0, thunder = 0, colpat = 0;
    u16 intensity = 0;
    DeriveWeather(mode, thunder, intensity, colpat);
    const u16 drift = intensity > g_lastIntensity ? intensity - g_lastIntensity
                                                  : g_lastIntensity - intensity;
    const bool snowDrift = mode == g_lastMode &&
                           mode == static_cast<u8>(WeatherMode::Snow) && drift >= 16;
    if (mode != g_lastMode || snowDrift || stageChanged) {
        g_lastMode = mode;
        g_lastIntensity = intensity;
        SendWeatherChange(mode, thunder, intensity, colpat);
    }

    // World info for joiners (JoinAccept/WorldInit, task 6).
    TimeStateInfo t = {};
    t.time = now;
    t.day = day;
    t.rate = rate;
    t.flags = flags;
    WeatherStateInfo w = {};
    w.mode = mode;
    w.thunder = thunder;
    w.intensity = intensity;
    w.colpat = colpat;
    dusk::coop::setWorldTime(t);
    dusk::coop::setWorldWeather(w);
}

// ---------------------------------------------------------------------------
// Client replica (04 §6.3; task 3)
// ---------------------------------------------------------------------------

/// Absolute per-tick advance implied by a rate bucket. Pond (rate 3) follows
/// the vanilla per-segment double/triple rule exactly (04 §5.11) when the
/// stage says so — same stage data both sides — with the uniform-2x
/// approximation only as a fallback.
f32 AdvanceStep() {
    const u8 rate = g_time.valid ? g_time.rate : kTimeRateFrozen;
    switch (rate) {
    case kTimeRateNormal:
        return 0.012f;
    case kTimeRateFast:
        return 1.0f;  // wolf-howl skip (04 §5.4)
    case kTimeRatePond2x: {
        const char* stage = dComIfGp_getStartStageName();
        if (stage != nullptr &&
            (std::strcmp(stage, "F_SP127") == 0 || std::strcmp(stage, "R_SP127") == 0))
        {
            // (u32)daytime >= 360 || <= 60 -> triple, 150..195 -> double;
            // mirrors setDaytime()'s vanilla pond block.
            if (g_env_light.daytime >= 300.0f || g_env_light.daytime <= 60.0f) {
                return 0.036f;
            }
            if (g_env_light.daytime >= 150.0f && g_env_light.daytime <= 195.0f) {
                return 0.024f;
            }
        }
        return 0.024f;  // uniform 2x fallback (04 §5.11)
    }
    default:
        return 0.0f;  // frozen
    }
}

// ---------------------------------------------------------------------------
// Client weather replica (04 §6.3; task 4)
// ---------------------------------------------------------------------------

/// dice_rain_minus (d_a_kytag06.cpp): teardown — every 4 frames, drain fast
/// while far, one at a time near zero. Runs on the local frame counter; the
/// aggregate drain rate matches the host's so the visual teardown timing
/// lines up even though the 4-frame phases differ between machines.
static void DiceRainMinus(int* r) {
    if ((g_Counter.mCounter0 & 3) == 0) {
        if (*r > 40) {
            *r -= 3;
        } else if (*r != 0) {
            (*r)--;
        }
    }
}

/// Re-runs the vanilla dice ramp toward the synced mode's canonical target
/// (04 §4.3 "same rules as dKy_event_proc"; R7 ±1-3/frame): RainLight holds
/// ~40, RainHeavy/ThunderHeavy climb to 250 at +1/frame, everything else
/// drains. New saturating from an arbitrary current value (kytag00 shelter /
/// area tags may have modified it in the actor phase — the ramp CONVERGES
/// rather than re-pinning, so their local modulation survives to the next
/// dKyw_wether_move exactly as on the host).
void RampRainTowardTarget() {
    int r = g_env_light.raincnt;
    switch (static_cast<WeatherMode>(g_weather.mode)) {
    case WeatherMode::RainLight:
        if (r < 40) {
            r++;
        } else if (r > 40) {
            r--;
        }
        break;
    case WeatherMode::RainHeavy:
    case WeatherMode::ThunderHeavy:
        if (r < 250) {
            r++;
        }
        break;
    case WeatherMode::ThunderLight:
    case WeatherMode::Clear:
    case WeatherMode::Cloudy:
    case WeatherMode::Snow:
    default:
        DiceRainMinus(&r);
        break;
    }
    if (r != g_env_light.raincnt) {
        dKyw_rain_set(r);
    }
}

/// Full re-pin of the synced weather state (packet receipt / stage load): the
/// host's exact position, no ramp.
void PinWeather() {
    dScnKy_env_light_c& env = g_env_light;
    dKyw_rain_set(static_cast<int>(g_weather.intensity));
    if (static_cast<WeatherMode>(g_weather.mode) == WeatherMode::Snow) {
        env.mSnowCount = static_cast<int>(g_weather.intensity);
    }
    env.mThunderEff.mMode = g_weather.thunder;
    if (env.mColpatWeather != g_weather.colpat) {
        dKy_change_colpat(g_weather.colpat);
    }
}

/// Per-frame mThunderEff write mirroring the dice proc (dKy_event_proc):
/// thunder modes force 1, Cloudy forces 0, Clear only clears a 1 — so a
/// kytag00 thunder-area tag's mMode == 2 survives exactly as it does on the
/// host (whose dice writes happen later in the frame). Rain modes never touch
/// it (the host's dice doesn't write in those modes either); the receipt-side
/// PinWeather already set it to the host's value at the mode change.
void ThunderPerMode() {
    switch (static_cast<WeatherMode>(g_weather.mode)) {
    case WeatherMode::ThunderLight:
    case WeatherMode::ThunderHeavy:
        g_env_light.mThunderEff.mMode = 1;
        break;
    case WeatherMode::Cloudy:
        g_env_light.mThunderEff.mMode = 0;
        break;
    case WeatherMode::Clear:
        if (g_env_light.mThunderEff.mMode == 1) {
            g_env_light.mThunderEff.mMode = 0;
        }
        break;
    default:
        break;  // RainLight/RainHeavy/Snow: leave kytag00 area state alone
    }
}

/// Per-frame mSnowCount convergence toward the synced target (mode SNOW =
/// carried intensity, else 0). Convergent (not a pin) so kytag00 type-2 snow
/// area tags locally boost like on the host; the up/down steps approximate
/// the kytag06/kytag00 drive rates and the host's 1 Hz snow-drift refreshes
/// keep the synced target current.
void SnowPerFrame() {
    dScnKy_env_light_c& env = g_env_light;
    const int target = static_cast<WeatherMode>(g_weather.mode) == WeatherMode::Snow
                           ? static_cast<int>(g_weather.intensity)
                           : 0;
    int s = env.mSnowCount;
    if (s < target) {
        s += target - s < 12 ? target - s : 12;
    } else if (s > target) {
        s -= s - target < 4 ? s - target : 4;
    }
    env.mSnowCount = s;
}

/// Client re-seed from JoinAccept / WorldInit (session worldTime/weather).
void SeedTargets() {
    const TimeStateInfo& t = dusk::coop::worldTime();
    const WeatherStateInfo& w = dusk::coop::worldWeather();
    g_worldSeenTime = t;
    g_worldSeenWeather = w;
    g_time.time = t.time;
    g_time.day = t.day;
    g_time.rate = t.rate;
    g_time.flags = t.flags;
    g_time.valid = true;
    g_weather.mode = w.mode;
    g_weather.thunder = w.thunder;
    g_weather.intensity = w.intensity;
    g_weather.colpat = w.colpat;
    g_weather.valid = true;
    // Adopt into the save / sky immediately so a mid-game joiner starts with
    // the host's clock and sky (task 6); a stage-load envcolor_init /
    // dKyw_wether_init overwrite later in the same frame is re-clobbered by
    // onStageCreate / the per-frame force.
    dComIfGs_setTime(t.time);
    dComIfGs_setDate(t.day);
    PinWeather();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool clientClockReplica() {
    if (!ClientActive()) {
        return false;  // host / offline: vanilla advance unchanged
    }
    dScnKy_env_light_c& env = g_env_light;
    if (dKy_darkworld_check()) {
        // 04 §5.5: the client's OWN twilight clock is local story — keep
        // vanilla darkworld semantics (advance the local dark clock; daytime
        // pinned to 0, which is the correct fixed twilight lighting).
        env.dark_daytime += env.time_change_rate;
        if ((u32)env.dark_daytime >= 360.0f) {
            env.darktime_week++;
            env.dark_daytime = 0.0f;
        }
        env.daytime = 0.0f;
    } else if (g_time.valid && g_time.rate != kTimeRateFrozen) {
        // Replicate the absolute clock: ratePerTick per sim tick (each
        // exeKankyo invocation is one tick — setDaytime runs once per sim
        // tick, so this IS the tick counter).
        env.daytime += AdvanceStep();
        if ((u32)env.daytime >= 360.0f) {
            env.daytime = 0.0f;
            env.mDate++;
            // Local dKankyo_DayProc equivalent (d/d_kankyo_static.h's
            // static-weak clears temp bit 91 — Ordon Ranch day-1 flag).
            dComIfGs_offTmpBit((u16)dSv_event_tmp_flag_c::tempBitLabels[91]);
        }
    }
    // rate==0 / no target yet: the phase HOLDS in place — the next TimeSync
    // adoption snaps it (absolute self-correction, 04 §8), exactly matching
    // the host's frozen clock (events cutscenes, kytag11 tags, ...).

    if (env.daytime >= 360.0f) {
        env.daytime = 0.0f;  // vanilla tail clamp
    }
    dComIfGs_setTime(env.daytime);
    mDoAud_setHour(dKy_getdaytime_hour());
    mDoAud_setMinute(dKy_getdaytime_minute());
    mDoAud_setWeekday(dKy_get_dayofweek());
    dComIfGs_setDate(env.mDate);
    g_env_light.using_time_control_tag = 0;
    return true;  // caller skips the vanilla advance (and its DEBUG variants)
}

void clientWeatherForce() {
    if (!ClientActive() || !g_weather.valid) {
        return;
    }
    dScnKy_env_light_c& env = g_env_light;
    RampRainTowardTarget();
    SnowPerFrame();
    ThunderPerMode();
    if (env.mColpatWeather != g_weather.colpat) {
        dKy_change_colpat(g_weather.colpat);
    }
}

void onStageCreate() {
    if (!ClientActive()) {
        return;
    }
    // 04 §5.10: re-assert the replicated time before room-layer resolution
    // (dComIfG_get_timelayer reads dKy_daynight_check). The stage-init write
    // (stagInfo hour / nexttime / twilight old_time restore) is clobbered —
    // host time is authoritative. Worst case: one stale room layer until the
    // next room load (accepted v1).
    if (g_time.valid) {
        dComIfGs_setTime(g_time.time);
        dComIfGs_setDate(g_time.day);
    }
    // 04 §5.6: re-assert the synced weather before the first dKyw_wether_move
    // of the new stage (both sides' dKyw_wether_init zeroed everything). If
    // dKyeff_Create happens to run after dKy_Create and re-zeroes, the
    // per-frame clientWeatherForce (pre-exeKankyo, before wether_move in the
    // same frame) re-applies — belt and braces.
    if (g_weather.valid) {
        PinWeather();
    }
}

bool suppressDiceWeather() {
    // R16 verified: the kytag06 type-4 branch (dKy_event_proc) drives weather
    // state only (dice machine: raincnt/colpat/thunder + its own counters) —
    // no non-weather effects — so suppressing the whole draw call is safe on
    // synced clients (04 §5.7). The host runs it natively; the offline client
    // runs vanilla.
    return ClientActive();
}

void onGameMessage(MsgType type, const PayloadUnion& payload) {
    if (!ClientActive()) {
        return;
    }
    switch (type) {
    case MsgType::TimeSync: {
        const auto& s = payload.timeSync;
        g_time.time = s.time;
        g_time.day = s.day;
        g_time.rate = s.rate;
        g_time.flags = s.flags;
        g_time.valid = true;
        // Adopt the absolute phase into the save; setDaytime's replica reads
        // it at the top of the same frame (Session::Update runs before the
        // actor phase).
        dComIfGs_setTime(s.time);
        dComIfGs_setDate(s.day);
        break;
    }
    case MsgType::TimeEvent: {
        // Advisory (the phase in TimeSync already encodes the boundary): the
        // reliable event exists so local one-shots fire exactly once.
        if (static_cast<TimeEventId>(payload.timeEvent.eventId) == TimeEventId::NewDay) {
            // The host's day wrapped (its dKankyo_DayProc ran natively); run
            // the local equivalent exactly once. The replica's own wrap path
            // may also clear it — offTmpBit is idempotent.
            dComIfGs_offTmpBit((u16)dSv_event_tmp_flag_c::tempBitLabels[91]);
        }
        // DAWN/DUSK are stored for the future per-player forms subsystem
        // (deferred, M4+); nothing to apply in M3.
        break;
    }
    case MsgType::WeatherChange: {
        const auto& w = payload.weatherChange;
        g_weather.mode = w.mode;
        g_weather.thunder = w.thunder;
        g_weather.intensity = w.intensity;
        g_weather.colpat = w.colpat;
        g_weather.valid = true;
        PinWeather();  // re-pin on receipt only (R7); the ramp runs per frame
        break;
    }
    default:
        break;
    }
}

void onGameFrame() {
    if (!dusk::coop::sessionActive()) {
        g_clientSeeded = false;
        g_time.valid = false;
        g_weather.valid = false;
        g_worldSeenTime = {};
        g_worldSeenWeather = {};
        return;
    }
    if (dusk::coop::hostRole()) {
        PublishHostState();
        return;
    }
    // Client: JoinAccept/WorldInit updates to the session's world state
    // (including the roster-refresh re-broadcast on later joins) re-seed the
    // replica targets. Comparison is against g_worldSeen*, never g_time/
    // g_weather — a re-broadcast of the SAME world state must not clobber
    // fresher TimeSync/WeatherChange receipts.
    if (!g_clientSeeded) {
        g_clientSeeded = true;
        SeedTargets();
        return;
    }
    const TimeStateInfo& t = dusk::coop::worldTime();
    const WeatherStateInfo& w = dusk::coop::worldWeather();
    const bool timeChanged = t.time != g_worldSeenTime.time || t.day != g_worldSeenTime.day ||
                             t.rate != g_worldSeenTime.rate || t.flags != g_worldSeenTime.flags;
    const bool weatherChanged = w.mode != g_worldSeenWeather.mode ||
                                w.thunder != g_worldSeenWeather.thunder ||
                                w.intensity != g_worldSeenWeather.intensity ||
                                w.colpat != g_worldSeenWeather.colpat;
    if (timeChanged || weatherChanged) {
        SeedTargets();
    }
}

void shutdown() {
    g_time = {};
    g_weather = {};
    g_clientSeeded = false;
    g_stageSeeded = false;
    g_lastStage[0] = '\0';
    g_lastPhase = 0.0f;
    g_lastDay = 0;
    g_lastRate = 0xFF;
    g_lastMode = 0xFF;
    g_worldSeenTime = {};
    g_worldSeenWeather = {};
}

}  // namespace dusk::coop::timeweather
