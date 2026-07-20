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
    g_globalItems = {};
    refreshGlobalItemsFromSave();
    syncPlayer0FromSave();
    syncLoadout0FromSave();
    bottles::syncUnlockedFromSave();
}

void reset() { init(); }

PlayerResources& resources(PlayerId id) {
    COOP_ASSERT(isValidPlayer(id));
    return runtime().playerRuntime[id].resources;
}

PlayerLoadout& loadout(PlayerId id) {
    COOP_ASSERT(isValidPlayer(id));
    return runtime().playerRuntime[id].loadout;
}

bool hasGlobalItem(u8 itemNo) {
    if (g_globalItems[itemNo]) {
        return true;
    }
    // Fall back to original first-bit / inventory so P0 save remains authority.
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
    if (id == 0) {
        return static_cast<s16>(originalPlayer().getPlayerStatusA().getLife());
    }
    return resources(id).life;
}

void setLife(PlayerId id, s16 life) {
    if (id == 0) {
        auto& status = originalPlayer().getPlayerStatusA();
        if (life < 0) {
            life = 0;
        }
        const s16 maxL = static_cast<s16>(status.getMaxLife());
        if (life > maxL) {
            life = maxL;
        }
        status.setLife(static_cast<u16>(life));
        resources(0).life = life;
        return;
    }
    auto& res = resources(id);
    res.life = life;
    if (res.life < 0) {
        res.life = 0;
    }
    if (res.life > res.maxLife) {
        res.life = res.maxLife;
    }
}

u16 getMagic(PlayerId id) {
    if (id == 0) {
        return originalPlayer().getPlayerStatusA().getMagic();
    }
    return resources(id).magic;
}

void setMagic(PlayerId id, u16 magic) {
    if (id == 0) {
        auto& status = originalPlayer().getPlayerStatusA();
        if (magic > status.getMaxMagic()) {
            magic = status.getMaxMagic();
        }
        status.setMagic(static_cast<u8>(magic));
        resources(0).magic = magic;
        return;
    }
    auto& res = resources(id);
    res.magic = magic > res.maxMagic ? res.maxMagic : magic;
}

u16 getOil(PlayerId id) {
    if (id == 0) {
        return originalPlayer().getPlayerStatusA().getOil();
    }
    return resources(id).oil;
}

void setOil(PlayerId id, u16 oil) {
    if (id == 0) {
        auto& status = originalPlayer().getPlayerStatusA();
        if (oil > status.getMaxOil()) {
            oil = status.getMaxOil();
        }
        status.setOil(oil);
        resources(0).oil = oil;
        return;
    }
    auto& res = resources(id);
    res.oil = oil > res.maxOil ? res.maxOil : oil;
}

s16 getRupees(PlayerId id) {
    if (id == 0) {
        return static_cast<s16>(originalPlayer().getPlayerStatusA().getRupee());
    }
    return resources(id).rupees;
}

bool trySpendRupees(PlayerId id, s16 amount) {
    if (amount < 0) {
        return false;
    }
    if (id == 0) {
        auto& status = originalPlayer().getPlayerStatusA();
        if (status.getRupee() < static_cast<u16>(amount)) {
            return false;
        }
        status.setRupee(static_cast<u16>(status.getRupee() - amount));
        resources(0).rupees = static_cast<s16>(status.getRupee());
        return true;
    }
    auto& res = resources(id);
    if (res.rupees < amount) {
        return false;
    }
    res.rupees = static_cast<s16>(res.rupees - amount);
    return true;
}

bool tryAddRupees(PlayerId id, s16 amount) {
    if (id == 0) {
        auto& status = originalPlayer().getPlayerStatusA();
        const s32 next = static_cast<s32>(status.getRupee()) + amount;
        const s32 maxR = status.getRupeeMax();
        const u16 clamped = static_cast<u16>(next > maxR ? maxR : (next < 0 ? 0 : next));
        status.setRupee(clamped);
        resources(0).rupees = static_cast<s16>(clamped);
        return true;
    }
    auto& res = resources(id);
    const s32 next = static_cast<s32>(res.rupees) + amount;
    res.rupees = static_cast<s16>(next > res.maxRupees ? res.maxRupees : (next < 0 ? 0 : next));
    return true;
}

u16 getArrows(PlayerId id) {
    if (id == 0) {
        return originalPlayer().getItemRecord().getArrowNum();
    }
    return resources(id).arrows;
}

bool tryConsumeArrow(PlayerId id) {
    if (id == 0) {
        auto& record = originalPlayer().getItemRecord();
        if (record.getArrowNum() == 0) {
            return false;
        }
        record.setArrowNum(static_cast<u8>(record.getArrowNum() - 1));
        resources(0).arrows = record.getArrowNum();
        return true;
    }
    auto& res = resources(id);
    if (res.arrows == 0) {
        return false;
    }
    --res.arrows;
    return true;
}

bool tryAddArrows(PlayerId id, u16 amount) {
    if (id == 0) {
        auto& record = originalPlayer().getItemRecord();
        const u16 maxA = originalPlayer().getItemMax().getArrowNum();
        const u32 next = static_cast<u32>(record.getArrowNum()) + amount;
        const u8 clamped = static_cast<u8>(next > maxA ? maxA : next);
        record.setArrowNum(clamped);
        resources(0).arrows = clamped;
        return true;
    }
    auto& res = resources(id);
    const u32 next = static_cast<u32>(res.arrows) + amount;
    res.arrows = static_cast<u16>(next > res.maxArrows ? res.maxArrows : next);
    return true;
}

u8 getPachinko(PlayerId id) {
    if (id == 0) {
        return originalPlayer().getItemRecord().getPachinkoNum();
    }
    return resources(id).pachinko;
}

bool tryConsumePachinko(PlayerId id) {
    if (id == 0) {
        auto& record = originalPlayer().getItemRecord();
        if (record.getPachinkoNum() == 0) {
            return false;
        }
        record.setPachinkoNum(static_cast<u8>(record.getPachinkoNum() - 1));
        resources(0).pachinko = record.getPachinkoNum();
        return true;
    }
    auto& res = resources(id);
    if (res.pachinko == 0) {
        return false;
    }
    --res.pachinko;
    return true;
}

bool tryAddPachinko(PlayerId id, u8 amount) {
    const u16 maxP = dComIfGs_getPachinkoMax();
    if (id == 0) {
        auto& record = originalPlayer().getItemRecord();
        const u16 next = static_cast<u16>(record.getPachinkoNum()) + amount;
        const u8 clamped = static_cast<u8>(next > maxP ? maxP : next);
        record.setPachinkoNum(clamped);
        resources(0).pachinko = clamped;
        return true;
    }
    auto& res = resources(id);
    const u16 next = static_cast<u16>(res.pachinko) + amount;
    res.pachinko = static_cast<u8>(next > maxP ? maxP : next);
    return true;
}

u8 getBombCount(PlayerId id, u8 bagIdx) {
    if (bagIdx >= 3) {
        return 0;
    }
    if (id == 0) {
        return originalPlayer().getItemRecord().getBombNum(bagIdx);
    }
    return resources(id).bombCounts[bagIdx];
}

bool tryConsumeBomb(PlayerId id, u8 bagIdx) {
    if (bagIdx >= 3) {
        return false;
    }
    if (id == 0) {
        auto& record = originalPlayer().getItemRecord();
        const u8 cur = record.getBombNum(bagIdx);
        if (cur == 0) {
            return false;
        }
        record.setBombNum(bagIdx, static_cast<u8>(cur - 1));
        resources(0).bombCounts[bagIdx] = static_cast<u8>(cur - 1);
        return true;
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
    if (id == 0) {
        auto& record = originalPlayer().getItemRecord();
        const u16 next = static_cast<u16>(record.getBombNum(bagIdx)) + amount;
        const u8 clamped = static_cast<u8>(next > maxBombs ? maxBombs : next);
        record.setBombNum(bagIdx, clamped);
        resources(0).bombCounts[bagIdx] = clamped;
        return true;
    }
    auto& res = resources(id);
    const u16 next = static_cast<u16>(res.bombCounts[bagIdx]) + amount;
    res.bombCounts[bagIdx] = static_cast<u8>(next > maxBombs ? maxBombs : next);
    return true;
}

void syncPlayer0FromSave() {
    auto& res = resources(0);
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
}

void syncPlayer0ToSave() {
    auto& res = resources(0);
    auto& status = originalPlayer().getPlayerStatusA();
    auto& record = originalPlayer().getItemRecord();

    // Capacities stay authoritative in original save; only write current consumables
    // and life/magic/oil/rupees for Player 0. Do not resize or rewrite unlocks here.
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
}

void syncLoadout0FromSave() {
    auto& lo = loadout(0);
    auto& status = originalPlayer().getPlayerStatusA();
    lo.itemX = status.getSelectItemIndex(SELECT_ITEM_X);
    lo.itemY = status.getSelectItemIndex(SELECT_ITEM_Y);
    lo.itemSelect = status.getSelectItemIndex(SELECT_ITEM_DOWN);
    lo.sword = status.getSelectEquip(COLLECT_SWORD);
    lo.shield = status.getSelectEquip(COLLECT_SHIELD);
    lo.armor = status.getSelectEquip(COLLECT_CLOTHING);
}

void syncLoadout0ToSave() {
    auto& lo = loadout(0);
    auto& status = originalPlayer().getPlayerStatusA();
    status.setSelectItemIndex(SELECT_ITEM_X, lo.itemX);
    status.setSelectItemIndex(SELECT_ITEM_Y, lo.itemY);
    status.setSelectItemIndex(SELECT_ITEM_DOWN, lo.itemSelect);
    status.setSelectEquip(COLLECT_SWORD, lo.sword);
    status.setSelectEquip(COLLECT_SHIELD, lo.shield);
    status.setSelectEquip(COLLECT_CLOTHING, lo.armor);
}

void initSecondaryFromGlobal(PlayerId id) {
    if (id == 0 || !isValidPlayer(id)) {
        return;
    }
    auto& res = resources(id);
    mirrorGlobalCapacities(res);

    // Starter consumables from global progression (not a copy of P0's current stack).
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

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC

extern "C" {

int dusk_coop_useResourceSidecar(void) {
    return dusk::coop::isEnabled() && dusk::coop::currentPlayer() != 0 ? 1 : 0;
}

u16 dusk_coop_getLife(void) {
    return static_cast<u16>(dusk::coop::inventory::getLife(dusk::coop::currentPlayer()));
}

void dusk_coop_setLife(u16 life) {
    dusk::coop::inventory::setLife(dusk::coop::currentPlayer(), static_cast<s16>(life));
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

u8 dusk_coop_getMagic(void) {
    return static_cast<u8>(dusk::coop::inventory::getMagic(dusk::coop::currentPlayer()));
}

void dusk_coop_setMagic(u8 magic) {
    dusk::coop::inventory::setMagic(dusk::coop::currentPlayer(), magic);
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

}  // extern "C"

#endif  // ENABLE_LOCAL_COOP && TARGET_PC
