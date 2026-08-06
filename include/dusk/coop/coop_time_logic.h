#pragma once

/**
 * \file coop_time_logic.h
 * M3.5 — game-free core of the time/weather sync decisions, shared verbatim
 * by the game TU (src/dusk/coop/coop_time.cpp adapts live engine state onto
 * these) and the dusk_net_selftest build (which table-tests them).
 *
 * Everything here is a pure function of its inputs — no game globals, no
 * engine symbols — so the selftest can exercise exactly the code paths that
 * shipped MAJOR 1 (the 60 Hz TimeSync cadence) and the M3.5 MINOR fixes
 * without linking the game (linking coop_time.cpp into the selftest would
 * require shimming g_dComIfG_gameInfo's constructor cascade plus ~16 engine
 * functions — not cheap, so the tested surface lives here instead).
 *
 * The selftest covers: the 1 Hz cadence gate (deepseek MAJOR 1), the
 * thunder-with-no-rain derive + defer-to-wire thunder policy (deepseek
 * MINOR 1), and the pond advance fallback (deepseek MINOR 3).
 */

#include "dolphin/types.h"
#include "dusk/net/protocol.h"

namespace dusk::coop::timeweather {

// ---------------------------------------------------------------------------
// Host weather derivation (04-time-weather.md §4.3) — pure mode decision.
// ---------------------------------------------------------------------------

/// Sky state snapshot a derive runs from (mirrors the g_env_light fields
/// coop_time.cpp reads).
struct SkyDeriveInput {
    int raincnt = 0;      // env.raincnt (0..250)
    int snowCount = 0;    // env.mSnowCount (>0 only in snow stages)
    u8 thunder = 0;       // env.mThunderEff.mMode != 0
    u8 colpat = 0;        // env.mColpatWeather
    bool diceStage = false;  // IsDiceStage(startStage): F_SP108/121/127
    u8 diceMode = 0;      // env.dice_wether_mode (DICE_MODE_* when diceStage)
};

/// Highest dice-machine mode whose entry is a direct WeatherMode mapping
/// (d_kankyo.h DICE_MODE_*_e); UNK6 (6) derives Clear.
static constexpr u8 kDiceModeThunderHeavy = 5;

inline u16 ClampU16(int v) {
    return v > 0xFFFF ? 0xFFFF : static_cast<u16>(v);
}

/// Derives the wire WeatherMode for the live sky state (04 §4.3): snow
/// first; dice stages map the dice machine directly; otherwise raincnt > 0
/// decides rain/thunder/drain, and no-rain falls to the palette (Cloudy/
/// Clear) — a stray thunder-without-rain collapses exactly as the dice
/// proc's own gate does. The caller still carries thunder/colpat/intensity
/// on the wire (see DeriveWeatherFrom), so a held thunder is not lost by
/// the mode collapsing (deepseek M1).
inline u8 DeriveWeatherMode(const SkyDeriveInput& in) {
    if (in.snowCount > 0) {
        return static_cast<u8>(net::WeatherMode::Snow);
    }
    if (in.diceStage && in.diceMode <= kDiceModeThunderHeavy) {
        // Direct dice-mode mapping (04 §4.3 table): Sunny->Clear, Cloudy->
        // Cloudy, RainLight->RainLight, RainHeavy->RainHeavy,
        // ThunderLight/Heavy->ThunderLight/Heavy, UNK6->Clear.
        static const u8 kDiceModeToWeather[7] = {
            static_cast<u8>(net::WeatherMode::Clear),
            static_cast<u8>(net::WeatherMode::Cloudy),
            static_cast<u8>(net::WeatherMode::RainLight),
            static_cast<u8>(net::WeatherMode::RainHeavy),
            static_cast<u8>(net::WeatherMode::ThunderLight),
            static_cast<u8>(net::WeatherMode::ThunderHeavy),
            static_cast<u8>(net::WeatherMode::Clear),
        };
        return kDiceModeToWeather[in.diceMode];
    }
    if (in.raincnt > 0) {
        if (in.thunder != 0) {
            return static_cast<u8>(in.colpat >= 2 ? net::WeatherMode::ThunderHeavy
                                                  : net::WeatherMode::ThunderLight);
        }
        if (in.colpat >= 2) {
            return static_cast<u8>(net::WeatherMode::RainHeavy);
        }
        if (in.colpat >= 1) {
            return static_cast<u8>(net::WeatherMode::RainLight);
        }
        return static_cast<u8>(net::WeatherMode::Clear);  // colpat 0 + rain = drain
    }
    // No rain, no snow: clear or cloudy from the palette (thunder only ever
    // coexists with colpat >= 1 in the engine, so a stray thunder-without-
    // rain collapses into Cloudy/Clear — same as the dice proc's gate).
    return static_cast<u8>(in.colpat >= 1 ? net::WeatherMode::Cloudy : net::WeatherMode::Clear);
}

/// Full wire payload of a derive: mode + thunder + colpat + intensity
/// (raincnt, or mSnowCount in snow stages).
struct DeriveResult {
    u8 mode = 0;
    u8 thunder = 0;
    u16 intensity = 0;
    u8 colpat = 0;
};

inline DeriveResult DeriveWeatherFrom(const SkyDeriveInput& in) {
    DeriveResult out;
    out.thunder = in.thunder != 0 ? 1 : 0;
    out.colpat = in.colpat;
    out.mode = DeriveWeatherMode(in);
    out.intensity = in.snowCount > 0 ? ClampU16(in.snowCount) : ClampU16(in.raincnt);
    return out;
}

// ---------------------------------------------------------------------------
// Client weather replica — thunder policy (deepseek M1 fix).
// ---------------------------------------------------------------------------

/// Returns the mThunderEff.mMode value a synced client should hold for the
/// current frame, given the wire (mode, thunder) and the local value.
/// Defer-to-wire rule (deepseek M1): modes that would clear thunder only
/// clear when the synced g_weather.thunder == 0. On dice stages the host's
/// Cloudy move clears thunder and publishes thunder=0; on non-dice stages
/// (boss arenas, daKytag06_wether_proc case 5) thunder can hold with rain
/// drained to 0 while the mode derives Cloudy — clearing on the wire's 1
/// would fight the host's flashing lightning (the old code undid the
/// PinWeather set on receipt).
inline u8 NextThunderMode(u8 wireMode, u8 wireThunder, u8 currentThunder) {
    switch (static_cast<net::WeatherMode>(wireMode)) {
    case net::WeatherMode::ThunderLight:
    case net::WeatherMode::ThunderHeavy:
        return 1;
    case net::WeatherMode::Cloudy:
        // Deepseek M1: only clear when the wire says 0. The wire's 1 means
        // the host's sky is holding thunder (wether-5 style) — re-assert it
        // like the host's own per-frame proc does.
        return wireThunder == 0 ? 0 : 1;
    case net::WeatherMode::Clear:
        // Clear only clears a 1 (kytag00 thunder-area tags' mMode == 2
        // survive exactly as on the host); with wire thunder held, keep it.
        return (currentThunder == 1 && wireThunder == 0) ? 0 : currentThunder;
    default:
        // RainLight/RainHeavy/Snow: never touch it (the host's dice doesn't
        // write in those modes either; kytag00 area state survives).
        return currentThunder;
    }
}

// ---------------------------------------------------------------------------
// Client clock replica — per-tick advance (04-time-weather.md §5.11).
// ---------------------------------------------------------------------------

/// Absolute per-tick advance for a rate bucket. Pond (kTimeRatePond2x) on a
/// pond stage follows the vanilla per-segment rule exactly (300-60 triple,
/// 150-195 double — setDaytime's pond block); the fallback is vanilla 1x
/// (deepseek M3: the old uniform-2x fallback over-advanced 0.012/tick for
/// 58% of the pond day).
inline f32 RatePerTick(u8 rate, f32 daytime, bool pondStage) {
    switch (rate) {
    case net::kTimeRateNormal:
        return 0.012f;
    case net::kTimeRateFast:
        return 1.0f;  // wolf-howl skip (04 §5.4)
    case net::kTimeRatePond2x:
        if (pondStage) {
            // (u32)daytime >= 360 || <= 60 -> triple, 150..195 -> double;
            // mirrors setDaytime()'s vanilla pond block.
            if (daytime >= 300.0f || daytime <= 60.0f) {
                return 0.036f;
            }
            if (daytime >= 150.0f && daytime <= 195.0f) {
                return 0.024f;
            }
        }
        return 0.012f;  // vanilla 1x outside the special windows (deepseek M3)
    default:
        return 0.0f;  // frozen
    }
}

// ---------------------------------------------------------------------------
// Client re-seed decision (deepseek MINOR B) — whether a session world-state
// re-seed (JoinAccept / WorldInit / roster-refresh re-broadcast) adopts the
// carried clock/sky. Pure decision shared with the selftest.
// ---------------------------------------------------------------------------

/// Inputs to a re-seed decision. `timeValid` is true once a TimeSync has been
/// received (g_time.valid); `timeChanged`/`weatherChanged` are true when the
/// session's world state differs from the last seen copy (the caller's own
/// change detection, incl. the first seed where both are true).
struct SeedInput {
    bool timeValid = false;
    bool timeChanged = false;
    bool weatherChanged = false;
};

struct SeedDecision {
    bool adoptTime = false;
    bool adoptWeather = false;
};

/// glm MINOR 1 (re-seed race) as a pure table: the clock is adopted only when
/// no TimeSync has been received yet — the first join, where the reliable
/// JoinAccept/WorldInit are the only time source and nothing can have raced
/// ahead. Once a TimeSync has landed (unreliable channel — it can overtake an
/// in-flight reliable WorldInit), a later roster-refresh WorldInit carrying an
/// OLDER time must not regress the clock, so re-seeds with `timeValid` do not
/// adopt time. Weather has no fresher per-frame stream (WeatherChange is
/// reliable like WorldInit, same channel, ordered), so it re-seeds
/// unconditionally on change.
inline SeedDecision SeedTargetsDecision(const SeedInput& in) {
    SeedDecision d;
    d.adoptTime = !in.timeValid && in.timeChanged;
    d.adoptWeather = in.weatherChanged;
    return d;
}

// ---------------------------------------------------------------------------
// Host publisher — WeatherChange publish decision (glm M3.5 MINOR 1).
// ---------------------------------------------------------------------------

/// Whether a WeatherChange should be published this frame. Modes drive most
/// publishes; SNOW also refreshes on meaningful intensity drift. The
/// thunder-only edge (glm M3.5 MINOR 1): a transition that flips
/// mThunderEff.mMode without changing the derived WeatherMode (kytag00
/// thunder-area tag arming on a stage whose colpat is already >= 1, wether-
/// proc case 5 with rain drained) publishes nothing under a mode-only gate,
/// so clients never learn the new thunder bit. `thunderChanged` covers it.
struct WeatherPublishInput {
    bool modeChanged = false;
    bool snowDrift = false;
    bool stageChanged = false;
    bool thunderChanged = false;
};

inline bool WeatherPublishDue(const WeatherPublishInput& in) {
    return in.modeChanged || in.snowDrift || in.stageChanged || in.thunderChanged;
}

// ---------------------------------------------------------------------------
// Host publisher — TimeSync cadence (deepseek MAJOR 1 fix).
// ---------------------------------------------------------------------------

/// Whether a TimeSync should be published this frame: the 1 Hz cadence tick,
/// an immediate send on stage change, or an immediate send on a rate
/// transition (04 §4.1 — clients stop fast-forwarding / resume promptly
/// instead of up to 1 s late). The immediate paths must survive the 1 Hz
/// gate; the selftest asserts both.
struct SyncDueInput {
    bool cadenceTick = false;  // net::NetClock::AtRate(1).Tick(nowUs)
    bool stageChanged = false;
    u8 rate = 0;
    u8 lastRate = 0;
};

inline bool TimeSyncDue(const SyncDueInput& in) {
    return in.cadenceTick || in.stageChanged || (in.rate != in.lastRate);
}

}  // namespace dusk::coop::timeweather
