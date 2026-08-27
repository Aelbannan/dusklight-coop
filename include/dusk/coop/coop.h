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
 * (docs/code-conventions.md); with no session (`connected` false, no
 * autoConnect launch) nothing here runs and single-player is byte-for-byte
 * vanilla.
 *
 * Wire surface: docs/design/mod-coop/00-network.md §5 (PlayerState raw-matrix
 * pose, Rev 3 D4; PlayerEvent on change) — see dusk/net/protocol.h.
 */

#include "dolphin/types.h"
#include "dusk/net/protocol.h"

class daAlink_c;
class daHorse_c;

namespace dusk::coop {

// ---------------------------------------------------------------------------
// Session glue (driven from duskExecute)
// ---------------------------------------------------------------------------

/// True when a network session is active (host listening or client joined).
bool sessionActive();
/// True when EnsureSession has started a session (including Connecting or
/// Rejected). Disconnect uses this; sessionActive() is the narrower
/// "currently playing" check. A remote end clears this so the Network tab
/// does not sit on a sticky Ended state.
bool sessionStarted();
/// This-process session intent (Host/Connect vs Disconnect). Not persisted.
bool connected();
/// True when this machine is the world host.
bool hostRole();
/// Start or restart a host session from the current net.* CVars. A live
/// session is stopped this frame and started on the next so puppets can
/// despawn (do not Stop+Start same frame). Does not set autoConnect.
void requestHost();
/// Start or restart a client session to joinHost:hostPort. Does not set
/// autoConnect.
void requestConnect();
/// Stop the session this process. Leaves net.autoConnect unchanged.
void requestDisconnect();
/// True when Host/Connect would tear down a live session (Listening /
/// Connecting / Connected / Joined). The Network tab confirms before that.
bool sessionWouldRestart();
/// True when already hosting with the current Port / Session Name — Host is
/// a no-op (do not restart / kick everyone).
bool hostingCurrentSettings();
/// True when already connecting or joined to the current Join Host IP / Port
/// / Session Name — Connect is a no-op.
bool connectingCurrentSettings();
/// Why Connect cannot proceed (empty or colon in Join Host IP), or nullptr.
const char* joinTargetError();
/// False when net.hostPort is 0 (ephemeral bind — the Network tab forbids it).
bool hostPortValid();
/// Short status for the Settings Network tab (Disconnected / Hosting / ...).
/// Valid until the next call.
const char* sessionStatusLabel();
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
// Horse puppet (v11) — ridden visual only. Never occupies mPlayerPtr[1].
// ---------------------------------------------------------------------------

/// True when `horse` is a remote ridden-Epona puppet, including during create.
bool isHorsePuppet(const daHorse_c* horse);
/// Frozen horse update: paste the latest HorseState pose. No AI, no save
/// writes, no HUD, no colliders.
int horsePuppetExecute(daHorse_c* horse);
/// True when the horse puppet must not draw (rider hidden / other room).
bool horsePuppetDrawHidden(const daHorse_c* horse);
/// Called at the end of daHorse_c::create() (cPhs_COMPLEATE_e) for a puppet.
void onHorseCreated(daHorse_c* horse);
/// Called from ~daHorse_c() for a puppet.
void onHorseDestroyed(daHorse_c* horse);

// ---------------------------------------------------------------------------
// Per-frame pump (duskExecute)
// ---------------------------------------------------------------------------

/// Session lifecycle + roster diff + spawn/despawn + receive handling. Called
/// once per game frame under #if TARGET_PC.
void onGameFrame();

/// Graceful session teardown on game exit (wired into dusk::config::shutdown):
/// a client sends PlayerLeave, the host broadcasts SessionEnd.
void shutdown();

// ---------------------------------------------------------------------------
// Session accessors
// ---------------------------------------------------------------------------

/// Sends a game message into the session (host: host->all simulcast; client:
/// to the host, which star-relays). Puppet pose/events use this.
bool sendGameMessage(net::MsgType type, const net::PayloadUnion& payload);
/// True when a remote roster slot is present in the session.
bool rosterPresent(net::PlayerId pid);

}  // namespace dusk::coop
