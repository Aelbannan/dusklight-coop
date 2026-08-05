#pragma once

/**
 * \file coop.h
 * Game-side player replication (M1) — the puppet machinery and session glue.
 *
 * The net module (src/dusk/net) owns transport/session/protocol only; this
 * module is the first GAME-SIDE consumer. It owns the one `Session` the game
 * runs, drives it per frame, and maintains the per-stage puppet registry
 * (`daAlink_c* <-> session PlayerId`), the sender (reads the local Link's
 * frame-final pose after execute) and the puppet apply pipeline (drives a
 * frozen `daAlink_c` from received `PlayerState`).
 *
 * Vanilla files call into this module exclusively under `#if TARGET_PC`
 * (docs/code-conventions.md); with networking off (`net.enabled = false`)
 * nothing here runs and single-player is byte-for-byte vanilla.
 *
 * Wire surface: docs/design/mod-coop/00-network.md §5 (PlayerState raw-matrix
 * pose, Rev 3 D4; PlayerEvent on change) — see dusk/net/protocol.h.
 */

#include "dolphin/types.h"
#include "dusk/net/protocol.h"

class daAlink_c;

namespace dusk::coop {

// ---------------------------------------------------------------------------
// Session glue (gated by net.enabled; driven from duskExecute)
// ---------------------------------------------------------------------------

/// True when a network session is active (host listening or client joined).
bool sessionActive();
/// True when this machine is the world host.
bool hostRole();
/// Our session-wide PlayerId (0..kMaxLocalPlayers-1).
net::PlayerId selfId();
/// Number of remote players currently in the session roster.
int remoteCount();

// ---------------------------------------------------------------------------
// Puppet registry — called from daAlink_c under #if TARGET_PC
// ---------------------------------------------------------------------------

/// True when `link` is a puppet (remote player avatar) rather than the local
/// real Link. Valid during create (the registry knows the pid before create
/// completes) and until the puppet's destructor runs.
bool isPuppet(const daAlink_c* link);
/// The session PlayerId of the puppet owning pid, or kInvalidPlayerId.
net::PlayerId puppetPlayerId(fpc_ProcID pid);

/// Called at the end of daAlink_c::create() (cPhs_COMPLEATE_e): registers the
/// real Link, or flips the matching puppet entry to active.
void onLinkCreated(daAlink_c* link);
/// Called from ~daAlink_c(): clears the registry entry / real-Link tracking.
void onLinkDestroyed(daAlink_c* link);

// ---------------------------------------------------------------------------
// Sender / apply — called from daAlink_c::execute under #if TARGET_PC
// ---------------------------------------------------------------------------

/// Frozen puppet update: no input, no action state machine, no damage/death
/// procs; drives the puppet purely from the latest received PlayerState.
/// Returns the execute() result.
int puppetExecute(daAlink_c* link);

/// Sender: reads the real Link's frame-final pose (post-execute) and enqueues
/// PlayerState (+ PlayerEvent on change). No-op unless a session is active
/// with a remote in the same room.
void sendPlayerState(daAlink_c* link);

/// True when the puppet must not draw (remote player is in another room).
bool puppetDrawHidden(const daAlink_c* link);

// ---------------------------------------------------------------------------
// Per-frame pump (duskExecute)
// ---------------------------------------------------------------------------

/// Session lifecycle + roster diff + spawn/despawn + receive handling. Called
/// once per game frame under #if TARGET_PC.
void onGameFrame();

/// Graceful session teardown on game exit (wired into dusk::config::shutdown):
/// a client sends PlayerLeave, the host broadcasts SessionEnd.
void shutdown();

}  // namespace dusk::coop
