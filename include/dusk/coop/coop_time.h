#pragma once

/**
 * \file coop_time.h
 * M3 — time of day & weather sync. The world host owns one clock and one sky
 * (00-network.md §8); clients replicate. Mechanics per docs/design/mod-coop/
 * 04-time-weather.md (symbol/offset tables verified by the adversarial
 * reviews) and implementation-plan Rev 3 §5 M3.
 *
 * Wire surface (v5, 04 §4): TimeSync (unreliable-sequenced, 1 Hz, absolute
 * phase f32 0..360 + day + rate + flags), TimeEvent (reliable: NEW_DAY /
 * DAWN / DUSK), WeatherChange (reliable: mode + thunder + intensity +
 * colpat). JoinAccept/WorldInit carry the same state so a mid-game joiner
 * starts with the host's sky.
 *
 * Attachment points (all `#if TARGET_PC`, original code intact):
 *   d_kankyo.cpp        setDaytime()   -> clientClockReplica() replaces the
 *                                        vanilla advance on synced clients
 *   d_kankyo.cpp        dKy_Execute()  -> clientWeatherForce() pre-exeKankyo
 *   d_kankyo.cpp        dKy_Create()   -> onStageCreate() re-asserts time
 *                                        (pre room-layer resolution, 04 §5.10)
 *                                        + weather (before the first
 *                                        dKyw_wether_move, 04 §5.6)
 *   d_a_kytag06.cpp     daKytag06_Draw() -> suppressDiceWeather() kills the
 *                                        local dice machine (mType==4) on
 *                                        clients (04 §5.7; R16 verified: the
 *                                        type-4 branch drives weather only)
 *
 * Host publisher runs in dusk::coop::onGameFrame (host role, session live);
 * client receive handling runs from dusk::coop::OnGameMessage. Everything is
 * a no-op when net.enabled is false or no session is active — single-player
 * stays byte-for-byte vanilla.
 */

#include "dolphin/types.h"
#include "dusk/net/protocol.h"

namespace dusk::coop::timeweather {

// ---------------------------------------------------------------------------
// Per-frame pump / receive (wired into dusk::coop)
// ---------------------------------------------------------------------------

/// Host: publishes TimeSync (1 Hz + on stage/rate change), TimeEvent (day
/// wrap, 90/285 crossings), WeatherChange (mode change + stage change) and
/// pushes the current world time/weather into the session for joiners.
/// Client: re-seeds the local replica targets from JoinAccept/WorldInit
/// state. No-op when the session is not live.
void onGameFrame();

/// Client: applies received TimeSync (absolute adopt + rate/flags), TimeEvent
/// (advisory; NEW_DAY runs the local DayProc equivalent), WeatherChange
/// (target + re-pin). Host: consumes nothing (it generates these itself).
void onGameMessage(net::MsgType type, const net::PayloadUnion& payload);

/// Clears module state on session shutdown.
void shutdown();

// ---------------------------------------------------------------------------
// Vanilla-file hooks (TARGET_PC)
// ---------------------------------------------------------------------------

/// Called at the top of dScnKy_env_light_c::setDaytime() after the save read
/// (mDate/daytime). On a synced client replaces the vanilla clock advance:
/// replicate phase by `ratePerTick * simTicks` (each exeKankyo invocation is
/// one sim tick), handle the 360 wrap with the local dKankyo_DayProc
/// equivalent, keep the audio clock and save writes, reset
/// using_time_control_tag. Darkworld (04 §5.5): a client with its OWN
/// darkworld flag keeps vanilla darkworld semantics (local dark_daytime
/// clock); otherwise the synced daytime is forced. Returns true when the
/// replica handled the whole function (caller returns early).
bool clientClockReplica();

/// Pre-exeKankyo weather force (dKy_Execute, client role only): ramps
/// raincnt toward the synced mode's canonical target with the vanilla dice
/// rules (04 §4.3 — re-pin only on packet receipt, R7), pins mSnowCount /
/// mThunderEff.mMode / mColpatWeather (blended via dKy_change_colpat).
/// kytag00 area tags keep running in the actor phase and locally modulate the
/// synced base exactly as on the host (their writes survive to the next
/// dKyw_wether_move because the ramp converges instead of re-pinning).
void clientWeatherForce();

/// Post-dKy_Create (stage load): a synced client re-asserts the replicated
/// time into the save before room-layer resolution (04 §5.10 — worst case one
/// stale layer until the next room load, accepted) and re-asserts the synced
/// weather before the first dKyw_wether_move of the new stage (04 §5.6).
void onStageCreate();

/// daKytag06_Draw type-4 gate: true when the local dice-weather machine must
/// be suppressed (synced client; the host runs it natively, 04 §5.7).
bool suppressDiceWeather();

}  // namespace dusk::coop::timeweather
