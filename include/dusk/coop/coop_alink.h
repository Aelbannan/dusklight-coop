#pragma once

#include "dusk/coop/coop_types.h"

class daAlink_c;

namespace dusk::coop::alink {

// Every player owns a real daAlink_c and indexed create state.

void registerPendingSpawn(PlayerId player);
void noteSpawnProcess(PlayerId player, u32 processId);
void clearSpawn(PlayerId player);
void clearPendingSpawn();
void markSpawnComplete(PlayerId player);

// Resolve which indexed player slot owns this Link.
PlayerId resolveOwner(const daAlink_c* link);

// Per-player create wait flag for every engine slot.
BOOL& bgWaitFlag(PlayerId player);

PlayerId ownerOf(const daAlink_c* link);
bool isStoryAuthorityLink(const daAlink_c* link);

// Called whenever an indexed Link finishes creating.
void onLinkCreated(PlayerId id, daAlink_c* link);
void clearLinkOwner(PlayerId id, const daAlink_c* link);

// Fill stick/buttons from PlayerInputSnapshot. Returns true if applied.
bool applyInputSnapshot(daAlink_c* link);

}  // namespace dusk::coop::alink
