#pragma once

#include "dusk/coop/coop_types.h"

namespace dusk::coop::inventory {

void init();
void reset();

// Gate E: Player 0 mirrors original save; others use sidecars.
PlayerResources& resources(PlayerId id);
PlayerLoadout& loadout(PlayerId id);

bool hasGlobalItem(u8 itemNo);
void unlockGlobalItem(u8 itemNo);
void refreshGlobalItemsFromSave();

s16 getLife(PlayerId id);
void setLife(PlayerId id, s16 life);

u16 getMagic(PlayerId id);
void setMagic(PlayerId id, u16 magic);

u16 getOil(PlayerId id);
void setOil(PlayerId id, u16 oil);

s16 getRupees(PlayerId id);
bool trySpendRupees(PlayerId id, s16 amount);
bool tryAddRupees(PlayerId id, s16 amount);

u16 getArrows(PlayerId id);
bool tryConsumeArrow(PlayerId id);
bool tryAddArrows(PlayerId id, u16 amount);

u8 getPachinko(PlayerId id);
bool tryConsumePachinko(PlayerId id);
bool tryAddPachinko(PlayerId id, u8 amount);

u8 getBombCount(PlayerId id, u8 bagIdx);
bool tryConsumeBomb(PlayerId id, u8 bagIdx);
bool tryAddBombs(PlayerId id, u8 bagIdx, u8 amount);

void syncPlayer0FromSave();
void syncPlayer0ToSave();
void syncLoadout0FromSave();
void syncLoadout0ToSave();

// Late-join / missing companion: copy global capacities from P0, starter consumables.
void initSecondaryFromGlobal(PlayerId id);

}  // namespace dusk::coop::inventory
