#pragma once

/**
 * \file coop_context.h
 * D3 — enemy targeting context swap (host-side only).
 *
 * Enemy AI reads "the player" through inline accessors that resolve slot 0
 * (dComIfGp_getPlayer(0) via fopAcM_searchPlayerAngleY/Distance* in
 * f_op_actor_mng.h, daPy_getPlayerActorClass/daPy_getLinkPlayerActorClass in
 * d_a_player.h). On the host, a whitelisted enemy's execute is wrapped in a
 * ScopedEnemyTarget that makes those inlines resolve the NEAREST real player
 * (the host's Link or a remote puppet), so enemies chase and attack remote
 * players, not just the host. Clients do not need this — their enemies are
 * frozen.
 *
 * The vanilla inlines call dusk::coop::currentTargetPlayer() under
 * TARGET_PC; with no context active it returns the native slot-0 player, so
 * non-enemy code paths are byte-for-byte vanilla.
 */

#include "dolphin/types.h"

class fopAc_ac_c;
class cXyz;

namespace dusk::coop {

/// The player a vanilla inline player-accessor resolves to: the scoped
/// (enemy-context) player if one is active, else the native slot-0 player.
fopAc_ac_c* currentTargetPlayer();

/// Nearest real player to `pos`: the local real Link, or the nearest active
/// remote puppet (same room, not hidden). Returns the slot-0 player when no
/// session is live. Host-side only.
fopAc_ac_c* resolveNearestPlayer(const cXyz& pos);

/// RAII: pushed around a whitelisted enemy's execute on the host so its
/// inline player reads resolve to the nearest real player. Cheap: a
/// thread-local stack of one pointer.
class ScopedEnemyTarget {
public:
    explicit ScopedEnemyTarget(fopAc_ac_c* enemy);
    ~ScopedEnemyTarget();

    ScopedEnemyTarget(const ScopedEnemyTarget&) = delete;
    ScopedEnemyTarget& operator=(const ScopedEnemyTarget&) = delete;

private:
    fopAc_ac_c* previous_ = nullptr;
    bool pushed_ = false;
};

}  // namespace dusk::coop
