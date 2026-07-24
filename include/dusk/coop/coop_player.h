#pragma once

#include "dusk/coop/coop_types.h"

namespace dusk::coop::player {

void init();
void reset();
void tick();

// Every joined player owns a real daAlink_c in native indexed storage.
bool spawnPlayerLink(PlayerId id, const cXyz& pos, s16 yaw);
bool spawnPlayerLinkNearAuthority(PlayerId id);
void destroyPlayerLink(PlayerId id);
void destroyNonAuthorityLinks();

bool softSeparate(PlayerId a, PlayerId b);

void onRoomUnload();

// Called from Press-Start join after the slot is claimed.
bool onPlayerJoined(PlayerId id);

// Called when any indexed daAlink_c create completes.
void onLinkReady(PlayerId id);

}  // namespace dusk::coop::player
