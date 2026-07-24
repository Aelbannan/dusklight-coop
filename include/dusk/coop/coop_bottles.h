#pragma once

#include "dusk/coop/coop_types.h"

namespace dusk::coop::bottles {

void init();
void reset();

u8 unlockedSlotCount();  // global progression; contents are indexed per player
void setUnlockedSlotCount(u8 count);
void syncUnlockedFromSave();

u8 getContents(PlayerId id, u8 slot);
bool setContents(PlayerId id, u8 slot, u8 itemNo);
u8 getQuantity(PlayerId id, u8 slot);
bool setQuantity(PlayerId id, u8 slot, u8 qty);
bool tryConsume(PlayerId id, u8 slot);

// Collector gets initial contents; other joined players get empty unlocked bottles.
void onBottleUnlock(PlayerId collector, u8 slot, u8 initialContents);

// Find the next locked slot, unlock it globally, and give contents to the collector.
bool grantBottleUnlock(PlayerId collector, u8 initialContents);

}  // namespace dusk::coop::bottles
