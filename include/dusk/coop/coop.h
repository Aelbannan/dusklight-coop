#pragma once

#include "dusk/coop/coop_types.h"

namespace dusk::coop {

Runtime& runtime();


void init();
void reset();
void tick();
void onRoomUnload();
void onCoopDisable();

PlayerId activePlayer();
ViewId activeView();

bool isValidPlayer(PlayerId id);
bool isValidView(ViewId id);
bool isJoined(PlayerId id);

PlayerSlot* playerSlot(PlayerId id);
PlayerRuntime* playerRuntime(PlayerId id);
HorseSlot* horseSlot(PlayerId id);
CameraRoute* cameraRoute(ViewId id);

}  // namespace dusk::coop
