#include "dusk/coop/coop_context.h"

#include "dusk/coop/coop.h"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"

#include <aurora/lib/logging.hpp>

#include <vector>

namespace dusk::coop {
namespace {

aurora::Module ContextLog("dusk::coop::context");

// Thread-local scoped-player stack. Only the game thread pushes/pops, so the
// thread_local keeps the state safe even if a future build drives the session
// from another thread.
thread_local std::vector<fopAc_ac_c*> g_stack;

}  // namespace

fopAc_ac_c* currentTargetPlayer() {
    if (!g_stack.empty()) {
        return g_stack.back();
    }
    // Native path: no enemy targeting context active.
    return dComIfGp_getPlayer(0);
}

fopAc_ac_c* resolveNearestPlayer(const cXyz& pos) {
    if (!dusk::coop::sessionActive()) {
        return dComIfGp_getPlayer(0);
    }

    fopAc_ac_c* nearest = dComIfGp_getPlayer(0);  // the host's own Link
    f32 nearestDist = 1e30f;
    if (nearest != nullptr) {
        nearestDist = pos.absXZ(nearest->current.pos);
    }

    // Remote puppets: the coop player registry tracks them per stage; the
    // nearest same-room puppet wins over the host Link when it is closer.
    for (u8 i = 0; i < net::kMaxLocalPlayers; ++i) {
        if (i == dusk::coop::selfId()) {
            continue;
        }
        fopAc_ac_c* puppet = dusk::coop::puppetActorFor(i);
        if (puppet == nullptr ||
            dusk::coop::puppetDrawHidden(reinterpret_cast<daAlink_c*>(puppet)))
        {
            continue;
        }
        const f32 dist = pos.absXZ(puppet->current.pos);
        if (dist < nearestDist) {
            nearestDist = dist;
            nearest = puppet;
        }
    }
    return nearest;
}

ScopedEnemyTarget::ScopedEnemyTarget(fopAc_ac_c* enemy) {
    if (enemy == nullptr) {
        return;
    }
    previous_ = currentTargetPlayer();
    g_stack.push_back(resolveNearestPlayer(enemy->current.pos));
    pushed_ = true;
}

ScopedEnemyTarget::~ScopedEnemyTarget() {
    if (!pushed_) {
        return;
    }
    if (!g_stack.empty()) {
        g_stack.pop_back();
    }
    pushed_ = false;
    (void)previous_;  // restored implicitly by popping
}

}  // namespace dusk::coop
