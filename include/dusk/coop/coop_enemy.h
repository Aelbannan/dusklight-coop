#pragma once

/**
 * \file coop_enemy.h
 * M5.1 — enemy subsystem SHREDDED to a lifecycle + entity-id stub.
 *
 * The M2/M3/M4 authority stack (freeze/apply, whitelist adapters, synthetic
 * hit injection, drops, room-clear, room ownership) is DELETED per
 * 05-ghosts.md §2: in the parallel-worlds model every machine simulates its
 * own enemies natively with vanilla AI and there is no shared combat state.
 * What survives is the *entity-id scheme* (coop_entity_logic.h — kept and
 * repurposed for M5.2's ghost ids, 05-ghosts.md §4.1) and the per-stage
 * reset lifecycle. `registerDynamicEnemy` stays DORMANT here; the M5.2 ghost
 * sender activates it (dynamic spawns get owner-major ids).
 *
 * M5.1 acceptance: zero enemy/ghost senders, zero registry growth from game
 * traffic, vanilla fopAc_Execute, vanilla ALLDIE. The ghost sender (M5.2)
 * and receiver (M5.3) replace this module as they land.
 */

#include "dolphin/types.h"
#include "dusk/coop/coop_entity_logic.h"
#include "dusk/net/protocol.h"

class fopAc_ac_c;

namespace dusk::coop::enemy {

// ---------------------------------------------------------------------------
// Registry — entityId <-> fopAc_ac_c* (kept for the M5.2 ghost sender)
// ---------------------------------------------------------------------------

// kInvalidEnemyId / StageEntityId / DynamicEntityId come from
// coop_entity_logic.h — the id scheme survives the shred unchanged
// (05-ghosts.md §4.1 "kept and repurposed").

/// Entity id for a stage-placed actor: (roomNo, setID) packed — every machine
/// has the same stage, so every machine derives identical ids (the ghost
/// receiver keys its registry on (senderId, entityId) instead, 05-ghosts.md
/// §4.1). kInvalidEnemyId when not registered.
u16 entityIdForActor(const fopAc_ac_c* actor);

/// Actor for an entity id (registered instances only), or nullptr.
fopAc_ac_c* actorForEntityId(u16 entityId);

/// True when the actor is registered (id assigned).
bool isRegistered(const fopAc_ac_c* actor);

/// Assigns an owner-major dynamic entity id to a non-stage-placed actor
/// (script-spawned enemies, projectiles). DORMANT in M5.1 — nothing calls it
/// yet; the M5.2 ghost sender activates it (05-ghosts.md §4.1/§4.4).
u16 registerDynamicEnemy(fopAc_ac_c* actor);

// ---------------------------------------------------------------------------
// Per-frame lifecycle (dusk::coop::onGameFrame / shutdown)
// ---------------------------------------------------------------------------

/// Per-frame: detects stage/room changes and resets the per-stage id tables.
/// With no live session it is a no-op (the net.enabled=false vanilla
/// guarantee — zero registry entries). The M2 registration scans, death
/// polls, snapshot sends and combat-intent flushing are GONE; M5.2 adds the
/// ghost sender's room scan back.
void onGameFrame();

/// Drops all per-stage state (id tables). Called from dusk::coop::shutdown.
void shutdown();

}  // namespace dusk::coop::enemy
