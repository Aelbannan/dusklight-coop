#include "dusk/coop/coop_accessors.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_context.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_horses.h"

#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"

namespace dusk::coop {

fopAc_ac_c* getPlayerActor(PlayerId id) {
    COOP_ASSERT(isValidPlayer(id));
    if (!isEnabled() || id == 0) {
        return dComIfGp_getPlayer(0);
    }
    auto* slot = playerSlot(id);
    return slot ? slot->actor : nullptr;
}

void setPlayerActor(PlayerId id, fopAc_ac_c* actor) {
    COOP_ASSERT(isValidPlayer(id));
    if (!isEnabled() || id == 0) {
        dComIfGp_setPlayer(0, actor);
        return;
    }
    if (auto* slot = playerSlot(id)) {
        slot->actor = actor;
    }
}

camera_class* getCameraProcess(ViewId id) {
    COOP_ASSERT(isValidView(id));
    if (!isEnabled() || id == 0) {
        return reinterpret_cast<camera_class*>(dComIfGp_getCamera(0));
    }
    auto* route = cameraRoute(id);
    return route ? route->process : nullptr;
}

void setCameraProcess(ViewId id, camera_class* cam) {
    COOP_ASSERT(isValidView(id));
    if (!isEnabled() || id == 0) {
        dComIfGp_getCamera(0);  // touch primary path
        // Primary camera registration remains through original play info.
        return;
    }
    if (auto* route = cameraRoute(id)) {
        route->process = cam;
    }
}

daHorse_c* getOwnedHorse(PlayerId id) {
    COOP_ASSERT(isValidPlayer(id));
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    if (isEnabled()) {
        if (id == 0) {
            // Always read the raw global singleton — never recurse through compat getHorseActor.
            return horses::getGlobalHorseActor();
        }
        auto* hs = horseSlot(id);
        return hs ? hs->actor : nullptr;
    }
#endif
    if (id == 0) {
        return dComIfGp_getHorseActor();
    }
    auto* hs = horseSlot(id);
    return hs ? hs->actor : nullptr;
}

void setOwnedHorse(PlayerId id, daHorse_c* horse) {
    COOP_ASSERT(isValidPlayer(id));
    if (!isEnabled() || id == 0) {
        dComIfGp_setHorseActor(reinterpret_cast<fopAc_ac_c*>(horse));
        if (auto* hs = horseSlot(0)) {
            hs->actor = horse;
            hs->owner = 0;
            hs->summoned = horse != nullptr;
            hs->actorId = horse != nullptr
                              ? fopAcM_GetID(reinterpret_cast<fopAc_ac_c*>(horse))
                              : fpcM_ERROR_PROCESS_ID_e;
        }
        return;
    }
    if (auto* hs = horseSlot(id)) {
        hs->actor = horse;
        hs->owner = id;
        hs->summoned = horse != nullptr;
        hs->actorId = horse != nullptr ? fopAcM_GetID(reinterpret_cast<fopAc_ac_c*>(horse))
                                       : fpcM_ERROR_PROCESS_ID_e;
        // Secondary horses must not call dComIfGp_setHorseActor.
    }
}

fopAc_ac_c* getPlayerActorCompat() {
    return getPlayerActor(currentPlayer());
}

daHorse_c* getHorseActorCompat() {
    return horses::resolveForContext();
}

uint32_t* playerStatusWords(PlayerId id) {
    COOP_ASSERT(isValidPlayer(id));
    if (!isEnabled() || id == 0) {
        // Original one-slot status — index must remain 0.
        COOP_ASSERT(id == 0 && "Original one-slot storage indexed with nonzero value");
        return nullptr;  // callers should use dComIfGp player status APIs for P0
    }
    return runtime().playerRuntime[id].status.statusWords.data();
}

}  // namespace dusk::coop
