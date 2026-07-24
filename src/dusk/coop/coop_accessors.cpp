#include "dusk/coop/coop_accessors.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_context.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_horses.h"
#include "dusk/coop/coop_player_bridge.h"

#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"

namespace dusk::coop {

fopAc_ac_c* getPlayerActor(PlayerId id) {
    COOP_ASSERT(isValidPlayer(id));
    return dComIfGp_getPlayer(id);
}

void setPlayerActor(PlayerId id, fopAc_ac_c* actor) {
    COOP_ASSERT(isValidPlayer(id));
    dComIfGp_setPlayerInfo(id, actor, static_cast<int>(id));
    dComIfGp_setLinkPlayer(id, actor);
}

camera_class* getCameraProcess(ViewId id) {
    COOP_ASSERT(isValidView(id));
    return reinterpret_cast<camera_class*>(dComIfGp_getCamera(id));
}

void setCameraProcess(ViewId id, camera_class* cam) {
    COOP_ASSERT(isValidView(id));
    dComIfGp_setCamera(id, cam);
    if (auto* route = cameraRoute(id)) {
        route->process = cam;
    }
}

daHorse_c* getOwnedHorse(PlayerId id) {
    COOP_ASSERT(isValidPlayer(id));
    return dComIfGp_getHorseActor(id);
}

void setOwnedHorse(PlayerId id, daHorse_c* horse) {
    COOP_ASSERT(isValidPlayer(id));
    dComIfGp_setHorseActor(id, reinterpret_cast<fopAc_ac_c*>(horse));
    if (auto* hs = horseSlot(id)) {
        hs->owner = id;
        hs->summoned = horse != nullptr;
    }
}

fopAc_ac_c* getPlayerActorCompat() {
    return getPlayerActor(currentPlayer());
}

fopAc_ac_c* playerForContextBridge() {
    return getPlayerActor(currentPlayer());
}

int playerContextIndexBridge() {
    return static_cast<int>(currentPlayer());
}

daHorse_c* getHorseActorCompat() {
    return horses::resolveForContext();
}

uint32_t* playerStatusWords(PlayerId id) {
    COOP_ASSERT(isValidPlayer(id));
    return g_dComIfG_gameInfo.play.getPlayerStatusWords(id);
}

}  // namespace dusk::coop
