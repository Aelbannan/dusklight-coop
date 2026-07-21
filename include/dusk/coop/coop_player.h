#pragma once

#include "dusk/coop/coop_types.h"

namespace dusk::coop::player {

void init();
void reset();
void tick();

// Phase 6: secondary daAlink_c (Gate D proxy path retired for join).
bool spawnSecondaryLink(PlayerId id, const cXyz& pos, s16 yaw);
bool spawnSecondaryLinkNearAuthority(PlayerId id);
void destroySecondaryLink(PlayerId id);
void destroyAllSecondaryLinks();

bool softSeparate(PlayerId a, PlayerId b);
bool tetherTeleportIfNeeded(PlayerId follower, PlayerId authority);

void onRoomUnload();

// Called from Press-Start join after the slot is claimed.
bool onPlayerJoined(PlayerId id);

// Called when secondary daAlink_c create completes.
void onSecondaryLinkReady(PlayerId id);

}  // namespace dusk::coop::player
