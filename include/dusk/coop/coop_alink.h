#pragma once

#include "dusk/coop/coop_types.h"

class daAlink_c;

namespace dusk::coop::alink {

// Phase 6: pending-spawn registry (do not encode player id in Link params).
void registerPendingSpawn(PlayerId player);
bool hasPendingSpawn();
PlayerId peekPendingOwner();
PlayerId consumePendingOwner();
void clearPendingSpawn();

// Create-state isolation so secondary Links never share P0's bgWaitFlg.
BOOL& secondaryBgWaitFlag();

// Set while a secondary daAlink_c is inside create/playerInit.
void setCreatingOwner(PlayerId id);
PlayerId creatingOwner();
bool isCreatingSecondary();

bool isSecondaryLink(const daAlink_c* link);
PlayerId ownerOf(const daAlink_c* link);

// Called when secondary create completes.
void onSecondaryCreated(PlayerId id, daAlink_c* link);

// Fill stick/buttons from PlayerInputSnapshot. Returns true if applied.
bool applyInputSnapshot(daAlink_c* link);

}  // namespace dusk::coop::alink
