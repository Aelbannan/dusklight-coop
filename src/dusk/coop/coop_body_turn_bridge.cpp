#include "dusk/coop/coop_body_turn_bridge.h"

#if TARGET_PC

#include "d/actor/d_a_player.h"  // daPy_getPlayerActorClass, daPy_py_c
#include "dusk/coop/coop.h"
#include "dusk/coop/coop_accessors.h"
#include "dusk/coop/coop_render.h"
#include "f_op/f_op_actor_mng.h" // fopAcM_searchPlayerAngleY, fopAcM_searchActorAngleY

extern "C" {

// ── helpers ──────────────────────────────────────────────────────────────────

// Returns the player ID that owns the active conversation presentation,
// or 0xFF (invalid) when no presentation is active.
static dusk::coop::PlayerId resolvePresentationOwner(void) {
    if (!dusk::coop::render::isConversationPresentationActive()) {
        return 0xFF;  // invalid — no presentation
    }
    return static_cast<dusk::coop::PlayerId>(
        dusk::coop::render::getConversationPresentationOwner());
}

// ── public API ───────────────────────────────────────────────────────────────

s16 dusk_coop_resolve_body_turn_angle(fopAc_ac_c* npc) {
    const dusk::coop::PlayerId owner = resolvePresentationOwner();
    if (owner < dusk::coop::MAX_LOCAL_PLAYERS && dusk::coop::isJoined(owner)) {
        fopAc_ac_c* ownerActor = dusk::coop::getPlayerActor(owner);
        if (ownerActor != nullptr) {
            return fopAcM_searchActorAngleY(npc, ownerActor);
        }
    }
    // Fallback: vanilla P1 angle
    return fopAcM_searchPlayerAngleY(npc);
}

fopAc_ac_c* dusk_coop_resolve_body_turn_actor(void) {
    const dusk::coop::PlayerId owner = resolvePresentationOwner();
    if (owner < dusk::coop::MAX_LOCAL_PLAYERS && dusk::coop::isJoined(owner)) {
        fopAc_ac_c* ownerActor = dusk::coop::getPlayerActor(owner);
        if (ownerActor != nullptr) {
            return ownerActor;
        }
    }
    // Fallback: vanilla P1
    return daPy_getPlayerActorClass();
}

BOOL dusk_coop_body_turn_angle_equals(fopAc_ac_c* npc, s16 curAngleY) {
    return curAngleY == dusk_coop_resolve_body_turn_angle(npc);
}

}  // extern "C"

#endif  // TARGET_PC