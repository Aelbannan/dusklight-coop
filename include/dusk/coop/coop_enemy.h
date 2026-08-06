#pragma once

/**
 * \file coop_enemy.h
 * M2 — enemy authority: registry, snapshots, freeze/apply, drops, room-clear.
 *
 * Wire surface: docs/design/mod-coop/00-network.md §5 EnemySnapshot /
 * EnemyEvent; mechanics: docs/design/mod-coop/03-enemies.md; the verified
 * per-type conclusions: docs/design/mod-coop/m2-design-notes.md.
 *
 * Authority model (v1 co-located): the world host sims its own room's
 * enemies. Whitelisted enemies are published as 60 Hz EnemySnapshots; a
 * client freezes its local whitelisted instances (fopAc_Execute pre-guard in
 * f_op_actor.cpp) and drives them purely from received state. Combat against
 * a puppet is forwarded as CombatIntent to the sim owner, which injects the
 * hit into the enemy's OWN damage handler (flag-injection) and broadcasts
 * CombatResult + EnemyEvent(died); each client then spawns its own drop and
 * deletes the puppet so ALLDIE scans stay in step. Bosses are ordinary
 * enemies whose died event carries the per-player switch mask.
 */

#include "dolphin/types.h"
#include "dusk/coop/coop_entity_logic.h"
#include "dusk/net/protocol.h"

class fopAc_ac_c;

namespace dusk::coop::enemy {

// ---------------------------------------------------------------------------
// Per-type adapter table (03-enemies.md §7, m2-design-notes.md §2)
// ---------------------------------------------------------------------------

/// Damage/death semantics (03-enemies.md §3.4).
enum class DamageSemantics : u8 {
    Hp = 0,        // health is real HP (cc_at_check / inline At_Check)
    HitCount = 1,  // Armos: health is meaningless, dies on hit count
    Special = 2,   // reaction-only or non-HP death path
};

/// One whitelist entry: the per-type surface the freeze/apply/combat paths
/// need, all through public members of the game classes.
struct NetEnemyAdapter {
    s16 procName;
    const char* name;
    DamageSemantics semantics;
    /// Enemy id passed to fopAcM_createItemFromEnemyID on death (0xFF = none).
    u8 dropTableId;
    /// The mPowerType this type's create sets (client-side intent power calc;
    /// the owner's instance carries the same value).
    u8 defaultPowerType;
    /// B_* profile: died event carries the per-player switch mask (D9).
    bool isBoss;
    /// Re-register the damage collider(s) (dCcS clears them every frame).
    void (*refreshColliders)(fopAc_ac_c*);
    /// Pose + anim-frame drive (setBaseMtx equivalent + morf frame).
    void (*driveModel)(fopAc_ac_c*, const net::EnemySnapshotMsg&);
    /// Owner-side hit injection (set the Tg hit flag + synthetic At obj).
    void (*injectHit)(fopAc_ac_c*, const net::CombatIntentMsg&);
    /// Post-execute death test (health <= 0 / hit count / state).
    bool (*isDead)(const fopAc_ac_c*);
    /// Boss/switch-grant source (0xFF = none), valid while the actor lives.
    u8 (*deathSwitchNo)(const fopAc_ac_c*);
};

/// Whitelist lookup by procName; nullptr for unlisted types (they run local
/// AI on every machine — documented v1 limitation).
const NetEnemyAdapter* findAdapter(s16 procName);
bool isWhitelistedType(s16 procName);

// ---------------------------------------------------------------------------
// Registry — enemyId <-> fopAc_ac_c*
// ---------------------------------------------------------------------------

// kInvalidEnemyId / StageEntityId / DynamicEntityId come from
// coop_entity_logic.h (M4: the id scheme is shared with the selftest so the
// ownership-transfer stability contract is table-tested).

/// Entity id for a stage-placed actor: (roomNo, setID) packed — every machine
/// has the same stage, so host and client derive identical ids without a
/// mapping message (03-enemies.md §3.1). Dynamic spawns (setID == 0xFFFF) get
/// an owner-assigned counter id. kInvalidEnemyId when not registered.
u16 entityIdForActor(const fopAc_ac_c* actor);

/// Actor for an enemy id (registered instances only), or nullptr.
fopAc_ac_c* actorForEntityId(u16 enemyId);

/// Host-side hit injection: builds the synthetic full At collider and sets
/// the enemy's damage-collider Tg hit flag so the enemy's OWN next execute
/// runs its authentic damage reaction (m2-design-notes.md §1).
void applyInjectedHit(fopAc_ac_c* actor, const net::CombatIntentMsg& intent);

/// True when the actor is a registered whitelisted enemy (frozen on clients,
/// snapshotted on the host).
bool isRegistered(const fopAc_ac_c* actor);

/// The registered enemy id of a non-stage key (dynamic spawns).
u16 registerDynamicEnemy(fopAc_ac_c* actor);

// ---------------------------------------------------------------------------
// fopAc_Execute hooks (src/f_op/f_op_actor.cpp, TARGET_PC)
// ---------------------------------------------------------------------------

/// Client-side freeze/apply: applies the latest EnemySnapshot for the actor
/// and returns true (the caller skips the vanilla dispatch — AI never runs).
/// False means "vanilla" (not a registered puppet, no live session, host).
bool puppetExecute(fopAc_ac_c* actor);

/// Host-side: true when the actor's execute should run inside a targeting
/// context (whitelisted, registered, live session, this machine is the host).
bool hostNeedsContext(const fopAc_ac_c* actor);

/// Host-side post-dispatch: marks the enemy snapshot dirty and captures
/// death signals (boss switch) while the actor still lives.
void hostOnExecuted(fopAc_ac_c* actor);

// ---------------------------------------------------------------------------
// Per-frame pump (dusk::coop::onGameFrame)
// ---------------------------------------------------------------------------

void onGameFrame();
void shutdown();

// ---------------------------------------------------------------------------
// Game-message routing (dusk::coop::OnGameMessage)
// ---------------------------------------------------------------------------

void onGameMessage(net::MsgType type, const net::PayloadUnion& payload);

// ---------------------------------------------------------------------------
// Room-clear (src/d/actor/d_a_alldie.cpp, TARGET_PC)
// ---------------------------------------------------------------------------

/// Client-side ALLDIE gate: true while the ACT_CHECK -> ACT_TIMER transition
/// must wait for the owner's per-room roomClear bit (03-enemies.md §5).
bool clientRoomClearGated(s8 roomNo);

/// Host-side: the room's vanilla scan came up empty — broadcast the per-room
/// EnemyEvent(RoomClear) once.
void hostRoomCleared(s8 roomNo);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

}  // namespace dusk::coop::enemy
