#include "dusk/coop/coop_bottles.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_inventory.h"

#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_save.h"

namespace dusk::coop::bottles {
namespace {

u8 g_unlockedSlots = 0;

u8 countUnlockedInSave() {
    u8 count = 0;
    auto& items = g_dComIfG_gameInfo.info.getPlayer().getItem();
    for (int i = 0; i < dSv_player_item_c::BOTTLE_MAX; ++i) {
        const u8 item = items.getItem(i + SLOT_11, true);
        if (item != dItemNo_NONE_e) {
            ++count;
        }
    }
    return count;
}

}  // namespace

void init() {
    g_unlockedSlots = 0;
    syncUnlockedFromSave();
}

void reset() { init(); }

u8 unlockedSlotCount() { return g_unlockedSlots; }

void setUnlockedSlotCount(u8 count) {
    if (count > dSv_player_item_c::BOTTLE_MAX) {
        count = dSv_player_item_c::BOTTLE_MAX;
    }
    g_unlockedSlots = count;
}

void syncUnlockedFromSave() {
    g_unlockedSlots = countUnlockedInSave();

    // Mirror P0 bottle contents into sidecar via original save (not dComIfGs hooks).
    auto& items = g_dComIfG_gameInfo.info.getPlayer().getItem();
    auto& record = g_dComIfG_gameInfo.info.getPlayer().getItemRecord();
    for (u8 s = 0; s < dSv_player_item_c::BOTTLE_MAX; ++s) {
        inventory::resources(0).bottleContents[s] = items.getItem(s + SLOT_11, true);
        inventory::resources(0).bottleQuantities[s] = record.getBottleNum(s);
    }
}

u8 getContents(PlayerId id, u8 slot) {
    if (!isValidPlayer(id) || slot >= dSv_player_item_c::BOTTLE_MAX) {
        return dItemNo_NONE_e;
    }
    return inventory::resources(id).bottleContents[slot];
}

bool setContents(PlayerId id, u8 slot, u8 itemNo) {
    if (!isValidPlayer(id) || slot >= g_unlockedSlots) {
        return false;
    }
    inventory::resources(id).bottleContents[slot] = itemNo;
    if (id == 0) {
        g_dComIfG_gameInfo.info.getPlayer().getItem().setItem(slot + SLOT_11, itemNo);
    }
    return true;
}

u8 getQuantity(PlayerId id, u8 slot) {
    if (!isValidPlayer(id) || slot >= dSv_player_item_c::BOTTLE_MAX) {
        return 0;
    }
    return inventory::resources(id).bottleQuantities[slot];
}

bool setQuantity(PlayerId id, u8 slot, u8 qty) {
    if (!isValidPlayer(id) || slot >= g_unlockedSlots) {
        return false;
    }
    inventory::resources(id).bottleQuantities[slot] = qty;
    if (id == 0) {
        g_dComIfG_gameInfo.info.getPlayer().getItemRecord().setBottleNum(slot, qty);
    }
    return true;
}

bool tryConsume(PlayerId id, u8 slot) {
    if (!isValidPlayer(id) || slot >= g_unlockedSlots) {
        return false;
    }
    auto& contents = inventory::resources(id).bottleContents[slot];
    if (contents == dItemNo_NONE_e || contents == dItemNo_EMPTY_BOTTLE_e) {
        return false;
    }
    contents = dItemNo_EMPTY_BOTTLE_e;
    inventory::resources(id).bottleQuantities[slot] = 0;
    if (id == 0) {
        g_dComIfG_gameInfo.info.getPlayer().getItem().setItem(slot + SLOT_11,
                                                             dItemNo_EMPTY_BOTTLE_e);
        g_dComIfG_gameInfo.info.getPlayer().getItemRecord().setBottleNum(slot, 0);
    }
    return true;
}

void onBottleUnlock(PlayerId collector, u8 slot, u8 initialContents) {
    if (slot >= dSv_player_item_c::BOTTLE_MAX) {
        return;
    }
    if (slot >= g_unlockedSlots) {
        g_unlockedSlots = static_cast<u8>(slot + 1);
    }

    // Global unlock: ensure original save has the slot (empty if collector is secondary).
    const u8 p0Contents = (collector == 0) ? initialContents : dItemNo_EMPTY_BOTTLE_e;
    g_dComIfG_gameInfo.info.getPlayer().getItem().setItem(slot + SLOT_11, p0Contents);
    inventory::resources(0).bottleContents[slot] = p0Contents;

    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (!isJoined(i)) {
            continue;
        }
        if (i == collector) {
            inventory::resources(i).bottleContents[slot] = initialContents;
        } else if (i != 0) {
            inventory::resources(i).bottleContents[slot] = dItemNo_EMPTY_BOTTLE_e;
        }
        inventory::resources(i).bottleQuantities[slot] = 0;
    }

    debug::logInfo("bottle slot %u unlocked; collector=P%u contents=0x%02X", slot, collector,
                   initialContents);
}

bool grantBottleUnlock(PlayerId collector, u8 initialContents) {
    auto& items = g_dComIfG_gameInfo.info.getPlayer().getItem();
    for (u8 slot = 0; slot < dSv_player_item_c::BOTTLE_MAX; ++slot) {
        if (items.getItem(slot + SLOT_11, true) == dItemNo_NONE_e) {
            onBottleUnlock(collector, slot, initialContents);
            return true;
        }
    }
    return false;
}

}  // namespace dusk::coop::bottles
