#include "dusk/coop/coop_inventory.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_bottles.h"
#include "dusk/coop/coop_context.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_resource_bridge.h"

#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_save.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace dusk::coop::inventory {
namespace {

std::array<bool, 256> g_globalItems{};
bool g_ready = false;

dSv_player_c& originalPlayer() {
    return g_dComIfG_gameInfo.info.getPlayer();
}

void mirrorGlobalCapacities(PlayerResources& res) {
    auto& status = originalPlayer().getPlayerStatusA();
    auto& itemMax = originalPlayer().getItemMax();

    res.maxLife = static_cast<s16>(status.getMaxLife());
    res.maxMagic = status.getMaxMagic();
    res.maxOil = status.getMaxOil();
    res.maxArrows = itemMax.getArrowNum();
    res.maxRupees = static_cast<s16>(status.getRupeeMax());
}

void clampToCapacities(PlayerResources& res) {
    if (res.life > res.maxLife) {
        res.life = res.maxLife;
    }
    if (res.life < 0) {
        res.life = 0;
    }
    if (res.magic > res.maxMagic) {
        res.magic = res.maxMagic;
    }
    if (res.oil > res.maxOil) {
        res.oil = res.maxOil;
    }
    if (res.arrows > res.maxArrows) {
        res.arrows = res.maxArrows;
    }
    if (res.rupees > res.maxRupees) {
        res.rupees = res.maxRupees;
    }
    if (res.rupees < 0) {
        res.rupees = 0;
    }
    if (res.pachinko > dComIfGs_getPachinkoMax()) {
        res.pachinko = dComIfGs_getPachinkoMax();
    }
    for (u8 i = 0; i < 3; ++i) {
        const u8 bagItem = dComIfGs_getItem(static_cast<int>(SLOT_15 + i), true);
        const u8 maxBombs = (bagItem == dItemNo_NONE_e || bagItem == dItemNo_BOMB_BAG_LV1_e)
                                ? 0
                                : dComIfGs_getBombMax(bagItem);
        if (res.bombCounts[i] > maxBombs) {
            res.bombCounts[i] = maxBombs;
        }
    }
}

}  // namespace

void init() {
    g_ready = false;
    g_globalItems = {};
    refreshGlobalItemsFromSave();
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        syncPlayerFromSave(id);
    }
    bottles::syncUnlockedFromSave();
    g_ready = true;
}

void reset() { init(); }

void syncAllFromSave() {
    const bool wasReady = g_ready;
    g_ready = false;
    refreshGlobalItemsFromSave();
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        syncPlayerFromSave(id);
    }
    bottles::syncUnlockedFromSave();
    g_ready = wasReady;
}

PlayerResources& resources(PlayerId id) {
    COOP_ASSERT(isValidPlayer(id));
    return runtime().playerRuntime[id].resources;
}

PlayerLoadout& loadout(PlayerId id) {
    COOP_ASSERT(isValidPlayer(id));
    return runtime().playerRuntime[id].loadout;
}

bool ready() { return g_ready; }

bool hasGlobalItem(u8 itemNo) {
    if (g_globalItems[itemNo]) {
        return true;
    }
    // Global progression remains in the vanilla save format.
    if (dComIfGs_isItemFirstBit(itemNo)) {
        g_globalItems[itemNo] = true;
        return true;
    }
    return false;
}

void unlockGlobalItem(u8 itemNo) {
    g_globalItems[itemNo] = true;
    dComIfGs_onItemFirstBit(itemNo);
}

void refreshGlobalItemsFromSave() {
    g_globalItems = {};
    for (int i = 0; i < 256; ++i) {
        if (dComIfGs_isItemFirstBit(static_cast<u8>(i))) {
            g_globalItems[static_cast<u8>(i)] = true;
        }
    }
}

s16 getLife(PlayerId id) {
    return resources(id).life;
}

void setLife(PlayerId id, s16 life) {
    auto& res = resources(id);
    res.life = life;
    if (res.life < 0) {
        res.life = 0;
    }
    if (res.life > res.maxLife) {
        res.life = res.maxLife;
    }
}

s16 getMaxLife(PlayerId id) { return resources(id).maxLife; }

void setMaxLife(PlayerId id, s16 maxLife) {
    auto& res = resources(id);
    res.maxLife = std::max<s16>(0, maxLife);
    if (res.life > res.maxLife) {
        res.life = res.maxLife;
    }
    if (const auto* slot = playerSlot(id); slot != nullptr && slot->transitionAuthority) {
        originalPlayer().getPlayerStatusA().setMaxLife(static_cast<u8>(res.maxLife));
    }
}

u16 getMagic(PlayerId id) {
    return resources(id).magic;
}

void setMagic(PlayerId id, u16 magic) {
    auto& res = resources(id);
    res.magic = magic > res.maxMagic ? res.maxMagic : magic;
}

u16 getMaxMagic(PlayerId id) { return resources(id).maxMagic; }

void setMaxMagic(PlayerId id, u16 maxMagic) {
    auto& res = resources(id);
    res.maxMagic = maxMagic;
    if (res.magic > res.maxMagic) {
        res.magic = res.maxMagic;
    }
    if (const auto* slot = playerSlot(id); slot != nullptr && slot->transitionAuthority) {
        originalPlayer().getPlayerStatusA().setMaxMagic(static_cast<u8>(maxMagic));
    }
}

u16 getOil(PlayerId id) {
    return resources(id).oil;
}

void setOil(PlayerId id, u16 oil) {
    auto& res = resources(id);
    res.oil = oil > res.maxOil ? res.maxOil : oil;
}

u16 getMaxOil(PlayerId id) { return resources(id).maxOil; }

void setMaxOil(PlayerId id, u16 maxOil) {
    auto& res = resources(id);
    res.maxOil = maxOil;
    if (res.oil > res.maxOil) {
        res.oil = res.maxOil;
    }
    if (const auto* slot = playerSlot(id); slot != nullptr && slot->transitionAuthority) {
        originalPlayer().getPlayerStatusA().setMaxOil(maxOil);
    }
}

s16 getRupees(PlayerId id) {
    return resources(id).rupees;
}

bool trySpendRupees(PlayerId id, s16 amount) {
    if (amount < 0) {
        return false;
    }
    auto& res = resources(id);
    if (res.rupees < amount) {
        return false;
    }
    res.rupees = static_cast<s16>(res.rupees - amount);
    return true;
}

bool tryAddRupees(PlayerId id, s16 amount) {
    auto& res = resources(id);
    const s32 next = static_cast<s32>(res.rupees) + amount;
    res.rupees = static_cast<s16>(next > res.maxRupees ? res.maxRupees : (next < 0 ? 0 : next));
    return true;
}

u16 getArrows(PlayerId id) {
    return resources(id).arrows;
}

u16 getMaxArrows(PlayerId id) { return resources(id).maxArrows; }

void setMaxArrows(PlayerId id, u16 maxArrows) {
    auto& res = resources(id);
    res.maxArrows = maxArrows;
    if (res.arrows > res.maxArrows) {
        res.arrows = res.maxArrows;
    }
    if (const auto* slot = playerSlot(id); slot != nullptr && slot->transitionAuthority) {
        originalPlayer().getItemMax().setArrowNum(static_cast<u8>(maxArrows));
    }
}

bool tryConsumeArrow(PlayerId id) {
    auto& res = resources(id);
    if (res.arrows == 0) {
        return false;
    }
    --res.arrows;
    return true;
}

bool tryAddArrows(PlayerId id, u16 amount) {
    auto& res = resources(id);
    const u32 next = static_cast<u32>(res.arrows) + amount;
    res.arrows = static_cast<u16>(next > res.maxArrows ? res.maxArrows : next);
    return true;
}

u8 getPachinko(PlayerId id) {
    return resources(id).pachinko;
}

bool tryConsumePachinko(PlayerId id) {
    auto& res = resources(id);
    if (res.pachinko == 0) {
        return false;
    }
    --res.pachinko;
    return true;
}

bool tryAddPachinko(PlayerId id, u8 amount) {
    const u16 maxP = dComIfGs_getPachinkoMax();
    auto& res = resources(id);
    const u16 next = static_cast<u16>(res.pachinko) + amount;
    res.pachinko = static_cast<u8>(next > maxP ? maxP : next);
    return true;
}

u8 getBombCount(PlayerId id, u8 bagIdx) {
    if (bagIdx >= 3) {
        return 0;
    }
    return resources(id).bombCounts[bagIdx];
}

bool tryConsumeBomb(PlayerId id, u8 bagIdx) {
    if (bagIdx >= 3) {
        return false;
    }
    auto& count = resources(id).bombCounts[bagIdx];
    if (count == 0) {
        return false;
    }
    --count;
    return true;
}

bool tryAddBombs(PlayerId id, u8 bagIdx, u8 amount) {
    if (bagIdx >= 3) {
        return false;
    }
    const u8 bagItem = originalPlayer().getItem().getItem(static_cast<int>(SLOT_15 + bagIdx), true);
    const u8 maxBombs = (bagItem == dItemNo_NONE_e || bagItem == dItemNo_BOMB_BAG_LV1_e)
                            ? 0
                            : dComIfGs_getBombMax(bagItem);
    auto& res = resources(id);
    const u16 next = static_cast<u16>(res.bombCounts[bagIdx]) + amount;
    res.bombCounts[bagIdx] = static_cast<u8>(next > maxBombs ? maxBombs : next);
    return true;
}

void syncPlayerFromSave(PlayerId id) {
    COOP_ASSERT(isValidPlayer(id));
    auto& res = resources(id);
    auto& status = originalPlayer().getPlayerStatusA();
    auto& record = originalPlayer().getItemRecord();

    res.life = static_cast<s16>(status.getLife());
    res.maxLife = static_cast<s16>(status.getMaxLife());
    res.magic = status.getMagic();
    res.maxMagic = status.getMaxMagic();
    res.oil = status.getOil();
    res.maxOil = status.getMaxOil();
    res.rupees = static_cast<s16>(status.getRupee());
    res.maxRupees = static_cast<s16>(status.getRupeeMax());

    res.arrows = record.getArrowNum();
    res.maxArrows = originalPlayer().getItemMax().getArrowNum();
    res.pachinko = record.getPachinkoNum();

    for (u8 i = 0; i < 3; ++i) {
        res.bombCounts[i] = record.getBombNum(i);
    }
    for (u8 i = 0; i < 4; ++i) {
        res.bottleContents[i] = originalPlayer().getItem().getItem(SLOT_11 + i, true);
        res.bottleQuantities[i] = record.getBottleNum(i);
    }

    auto& lo = loadout(id);
    lo.itemX = status.getSelectItemIndex(SELECT_ITEM_X);
    lo.itemY = status.getSelectItemIndex(SELECT_ITEM_Y);
    lo.itemSelect = status.getSelectItemIndex(SELECT_ITEM_DOWN);
    lo.sword = status.getSelectEquip(COLLECT_SWORD);
    lo.shield = status.getSelectEquip(COLLECT_SHIELD);
    lo.armor = status.getSelectEquip(COLLECT_CLOTHING);
}

void syncPlayerToSave(PlayerId id) {
    COOP_ASSERT(isValidPlayer(id));
    auto& res = resources(id);
    auto& status = originalPlayer().getPlayerStatusA();
    auto& record = originalPlayer().getItemRecord();

    status.setLife(static_cast<u16>(res.life < 0 ? 0 : res.life));
    status.setMagic(static_cast<u8>(res.magic > 0xFF ? 0xFF : res.magic));
    status.setOil(res.oil);
    status.setRupee(static_cast<u16>(res.rupees < 0 ? 0 : res.rupees));
    record.setArrowNum(static_cast<u8>(res.arrows > 0xFF ? 0xFF : res.arrows));
    record.setPachinkoNum(res.pachinko);
    for (u8 i = 0; i < 3; ++i) {
        record.setBombNum(i, res.bombCounts[i]);
    }
    for (u8 i = 0; i < 4; ++i) {
        originalPlayer().getItem().setItem(SLOT_11 + i, res.bottleContents[i]);
        record.setBottleNum(i, res.bottleQuantities[i]);
    }

    auto& lo = loadout(id);
    status.setSelectItemIndex(SELECT_ITEM_X, lo.itemX);
    status.setSelectItemIndex(SELECT_ITEM_Y, lo.itemY);
    status.setSelectItemIndex(SELECT_ITEM_DOWN, lo.itemSelect);
    status.setSelectEquip(COLLECT_SWORD, lo.sword);
    status.setSelectEquip(COLLECT_SHIELD, lo.shield);
    status.setSelectEquip(COLLECT_CLOTHING, lo.armor);
}

void initPlayerFromProgression(PlayerId id) {
    if (!isValidPlayer(id)) {
        return;
    }
    auto& res = resources(id);
    mirrorGlobalCapacities(res);

    // Starter consumables from global progression, independent of another player's stack.
    res.life = res.maxLife;
    res.magic = res.maxMagic;
    res.oil = res.maxOil;
    res.rupees = 0;
    res.arrows = 0;
    res.pachinko = 0;
    res.bombCounts = {};
    res.bottleQuantities = {};

    const u8 unlocked = bottles::unlockedSlotCount();
    for (u8 s = 0; s < 4; ++s) {
        if (s < unlocked) {
            res.bottleContents[s] = dItemNo_EMPTY_BOTTLE_e;
        } else {
            res.bottleContents[s] = dItemNo_NONE_e;
        }
    }

    // If bow is unlocked globally, grant a starter quiver fill for late joiners.
    if (hasGlobalItem(dItemNo_BOW_e) || dComIfGs_getItem(SLOT_4, true) != dItemNo_NONE_e) {
        res.arrows = std::min<u16>(30, res.maxArrows);
    }
    if (dComIfGs_getItem(SLOT_23, true) == dItemNo_PACHINKO_e) {
        res.pachinko = dComIfGs_getPachinkoMax();
    }
    for (u8 i = 0; i < 3; ++i) {
        const u8 bagItem = dComIfGs_getItem(static_cast<int>(SLOT_15 + i), true);
        if (bagItem != dItemNo_NONE_e && bagItem != dItemNo_BOMB_BAG_LV1_e) {
            res.bombCounts[i] = dComIfGs_getBombMax(bagItem);
        }
    }

    clampToCapacities(res);

    auto& lo = loadout(id);
    lo = {};
    lo.sword = originalPlayer().getPlayerStatusA().getSelectEquip(COLLECT_SWORD);
    lo.shield = originalPlayer().getPlayerStatusA().getSelectEquip(COLLECT_SHIELD);
    lo.armor = originalPlayer().getPlayerStatusA().getSelectEquip(COLLECT_CLOTHING);
}

}  // namespace dusk::coop::inventory

#if TARGET_PC

extern "C" {

int dusk_coop_resourcesReady(void) {
    return dusk::coop::inventory::ready() ? 1 : 0;
}

u16 dusk_coop_getLife(void) {
    return static_cast<u16>(dusk::coop::inventory::getLife(dusk::coop::currentPlayer()));
}

void dusk_coop_setLife(u16 life) {
    dusk::coop::inventory::setLife(dusk::coop::currentPlayer(), static_cast<s16>(life));
}

u16 dusk_coop_getMaxLife(void) {
    return static_cast<u16>(dusk::coop::inventory::getMaxLife(dusk::coop::currentPlayer()));
}

void dusk_coop_setMaxLife(u16 maxLife) {
    dusk::coop::inventory::setMaxLife(dusk::coop::currentPlayer(), static_cast<s16>(maxLife));
}

u16 dusk_coop_getRupee(void) {
    return static_cast<u16>(dusk::coop::inventory::getRupees(dusk::coop::currentPlayer()));
}

void dusk_coop_setRupee(u16 rupees) {
    auto& res = dusk::coop::inventory::resources(dusk::coop::currentPlayer());
    res.rupees = static_cast<s16>(rupees);
    if (res.rupees > res.maxRupees) {
        res.rupees = res.maxRupees;
    }
}

u16 dusk_coop_getOil(void) {
    return dusk::coop::inventory::getOil(dusk::coop::currentPlayer());
}

void dusk_coop_setOil(u16 oil) {
    dusk::coop::inventory::setOil(dusk::coop::currentPlayer(), oil);
}

u16 dusk_coop_getMaxOil(void) {
    return dusk::coop::inventory::getMaxOil(dusk::coop::currentPlayer());
}

void dusk_coop_setMaxOil(u16 maxOil) {
    dusk::coop::inventory::setMaxOil(dusk::coop::currentPlayer(), maxOil);
}

u8 dusk_coop_getMagic(void) {
    return static_cast<u8>(dusk::coop::inventory::getMagic(dusk::coop::currentPlayer()));
}

void dusk_coop_setMagic(u8 magic) {
    dusk::coop::inventory::setMagic(dusk::coop::currentPlayer(), magic);
}

u8 dusk_coop_getMaxMagic(void) {
    return static_cast<u8>(dusk::coop::inventory::getMaxMagic(dusk::coop::currentPlayer()));
}

void dusk_coop_setMaxMagic(u8 maxMagic) {
    dusk::coop::inventory::setMaxMagic(dusk::coop::currentPlayer(), maxMagic);
}

u8 dusk_coop_getArrowNum(void) {
    return static_cast<u8>(dusk::coop::inventory::getArrows(dusk::coop::currentPlayer()));
}

void dusk_coop_setArrowNum(u8 num) {
    auto& res = dusk::coop::inventory::resources(dusk::coop::currentPlayer());
    res.arrows = num;
    if (res.arrows > res.maxArrows) {
        res.arrows = res.maxArrows;
    }
}

u8 dusk_coop_getArrowMax(void) {
    return static_cast<u8>(dusk::coop::inventory::getMaxArrows(dusk::coop::currentPlayer()));
}

void dusk_coop_setArrowMax(u8 max) {
    dusk::coop::inventory::setMaxArrows(dusk::coop::currentPlayer(), max);
}

u8 dusk_coop_getPachinkoNum(void) {
    return dusk::coop::inventory::getPachinko(dusk::coop::currentPlayer());
}

void dusk_coop_setPachinkoNum(u8 num) {
    auto& res = dusk::coop::inventory::resources(dusk::coop::currentPlayer());
    const u8 maxP = dComIfGs_getPachinkoMax();
    res.pachinko = num > maxP ? maxP : num;
}

u8 dusk_coop_getBombNum(u8 bagIdx) {
    return dusk::coop::inventory::getBombCount(dusk::coop::currentPlayer(), bagIdx);
}

void dusk_coop_setBombNum(u8 bagIdx, u8 num) {
    if (bagIdx >= 3) {
        return;
    }
    dusk::coop::inventory::resources(dusk::coop::currentPlayer()).bombCounts[bagIdx] = num;
}

u8 dusk_coop_getBottleNum(u8 bottleIdx) {
    if (bottleIdx >= 4) {
        return 0;
    }
    return dusk::coop::inventory::resources(dusk::coop::currentPlayer()).bottleQuantities[bottleIdx];
}

void dusk_coop_setBottleNum(u8 bottleIdx, u8 num) {
    if (bottleIdx >= 4) {
        return;
    }
    dusk::coop::inventory::resources(dusk::coop::currentPlayer()).bottleQuantities[bottleIdx] = num;
}

u8 dusk_coop_getBottleItem(u8 bottleIdx) {
    return dusk::coop::bottles::getContents(dusk::coop::currentPlayer(), bottleIdx);
}

void dusk_coop_setBottleItem(u8 bottleIdx, u8 itemNo) {
    dusk::coop::bottles::setContents(dusk::coop::currentPlayer(), bottleIdx, itemNo);
}

// Per-player item counter bridging — bypasses the global mItemInfo counters
// so that rupees / life-flush are credited to the active player directly.

void dusk_coop_addRupee(s32 amount) {
    auto& res = dusk::coop::inventory::resources(dusk::coop::currentPlayer());
    s32 newRupees = static_cast<s32>(res.rupees) + amount;
    if (newRupees > res.maxRupees) {
        newRupees = res.maxRupees;
    }
    if (newRupees < 0) {
        newRupees = 0;
    }
    res.rupees = static_cast<s16>(newRupees);
}

void dusk_coop_addLifeCount(f32 hearts) {
    auto& res = dusk::coop::inventory::resources(dusk::coop::currentPlayer());
    s32 newLife = static_cast<s32>(res.life) + static_cast<s32>(hearts);
    if (newLife > res.maxLife) {
        newLife = res.maxLife;
    }
    if (newLife < 0) {
        newLife = 0;
    }
    res.life = static_cast<s16>(newLife);
}

// Per-player bottle bridging — operates on currentPlayer's bottleContents.

u8 dusk_coop_checkBottle(u8 itemNo) {
    const auto pid = dusk::coop::currentPlayer();
    u8 count = 0;
    for (u8 i = 0; i < dSv_player_item_c::BOTTLE_MAX; ++i) {
        if (dusk::coop::bottles::getContents(pid, i) == itemNo) {
            ++count;
        }
    }
    return count;
}

u8 dusk_coop_checkEmptyBottle(void) {
    return dusk_coop_checkBottle(dItemNo_EMPTY_BOTTLE_e);
}

int dusk_coop_checkInsectBottle(void) {
    const auto pid = dusk::coop::currentPlayer();
    for (u8 i = 0; i < dSv_player_item_c::BOTTLE_MAX; ++i) {
        const u8 content = dusk::coop::bottles::getContents(pid, i);
        if (content >= dItemNo_M_BEETLE_e && content <= dItemNo_F_MAYFLY_e) {
            return 1;
        }
    }
    return 0;
}

void dusk_coop_setBottleItemIn(u8 curItem, u8 newItem) {
    const auto pid = dusk::coop::currentPlayer();
    for (u8 i = 0; i < dSv_player_item_c::BOTTLE_MAX; ++i) {
        if (dusk::coop::bottles::getContents(pid, i) == curItem) {
            dusk::coop::bottles::setContents(pid, i, newItem);
            return;
        }
    }
}

void dusk_coop_setEmptyBottleItemIn(u8 itemNo) {
    dusk_coop_setBottleItemIn(dItemNo_EMPTY_BOTTLE_e, itemNo);
}

void dusk_coop_setEmptyBottle(void) {
    dusk::coop::bottles::grantBottleUnlock(dusk::coop::currentPlayer(), dItemNo_EMPTY_BOTTLE_e);
}

void dusk_coop_setEmptyBottleWithItem(u8 itemNo) {
    dusk::coop::bottles::grantBottleUnlock(dusk::coop::currentPlayer(), itemNo);
}

void dusk_coop_setEquipBottleItemIn(u8 curItem, u8 newItem) {
    const auto pid = dusk::coop::currentPlayer();
    const auto& lo = dusk::coop::inventory::loadout(pid);

    u8 slot = 0xFF;
    if (curItem == 0) slot = lo.itemX;
    else if (curItem == 1) slot = lo.itemY;
    else if (curItem == 2) slot = lo.itemSelect;

    if (slot >= SLOT_11 && slot <= SLOT_14) {
        const u8 bottleIdx = static_cast<u8>(slot - SLOT_11);
        dusk::coop::bottles::setContents(pid, bottleIdx, newItem);
        // Update the save slot for the story authority (per-player routing in dComIfGs_setItem).
        dComIfGs_setItem(slot, newItem);
        dComIfGp_setItem(slot, newItem);
        dComIfGp_setSelectItem(curItem);
    }
}

void dusk_coop_setEquipBottleItemEmpty(u8 curItem) {
    dusk_coop_setEquipBottleItemIn(curItem, dItemNo_EMPTY_BOTTLE_e);
}

}  // extern "C"

#endif  // TARGET_PC
