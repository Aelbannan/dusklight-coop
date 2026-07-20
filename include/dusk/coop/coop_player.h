#pragma once

#include "dusk/coop/coop_types.h"

namespace dusk::coop::player {

void init();
void reset();
void tick();

// Gate D: proxy secondary player before full daAlink_c dual spawn.
bool spawnProxy(PlayerId id, const cXyz& pos, s16 yaw);
bool spawnProxyNearAuthority(PlayerId id);
void destroyProxy(PlayerId id);
void destroyAllProxies();

bool softSeparate(PlayerId a, PlayerId b);
bool tetherTeleportIfNeeded(PlayerId follower, PlayerId authority);

void onRoomUnload();

// True when a proxy body exists (or spawn is pending after room unload).
bool hasProxy(PlayerId id);
fopAc_ac_c* getProxyActor(PlayerId id);

// Called from Press-Start join after the slot is claimed.
bool onPlayerJoined(PlayerId id);

}  // namespace dusk::coop::player
