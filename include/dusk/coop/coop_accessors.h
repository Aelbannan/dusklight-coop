#pragma once

#include "dusk/coop/coop_types.h"

namespace dusk::coop {

// Indexed accessors backed by the engine's native eight-player/eight-view storage.
fopAc_ac_c* getPlayerActor(PlayerId id);
void setPlayerActor(PlayerId id, fopAc_ac_c* actor);

camera_class* getCameraProcess(ViewId id);
void setCameraProcess(ViewId id, camera_class* cam);

daHorse_c* getOwnedHorse(PlayerId id);
void setOwnedHorse(PlayerId id, daHorse_c* horse);

// Compatibility wrappers for original global getters under co-op context.
fopAc_ac_c* getPlayerActorCompat();
daHorse_c* getHorseActorCompat();

uint32_t* playerStatusWords(PlayerId id);

}  // namespace dusk::coop
