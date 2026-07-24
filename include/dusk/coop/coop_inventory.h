#pragma once

#include "dusk/coop/coop_types.h"

namespace dusk::coop::inventory {

void init();
void reset();
void syncAllFromSave();

// Native indexed gameplay state for every player, including player 0.
PlayerResources& resources(PlayerId id);
PlayerLoadout& loadout(PlayerId id);
bool ready();

bool hasGlobalItem(u8 itemNo);
void unlockGlobalItem(u8 itemNo);
void refreshGlobalItemsFromSave();

s16 getLife(PlayerId id);
void setLife(PlayerId id, s16 life);
s16 getMaxLife(PlayerId id);
void setMaxLife(PlayerId id, s16 maxLife);

u16 getMagic(PlayerId id);
void setMagic(PlayerId id, u16 magic);
u16 getMaxMagic(PlayerId id);
void setMaxMagic(PlayerId id, u16 maxMagic);

u16 getOil(PlayerId id);
void setOil(PlayerId id, u16 oil);
u16 getMaxOil(PlayerId id);
void setMaxOil(PlayerId id, u16 maxOil);

s16 getRupees(PlayerId id);
bool trySpendRupees(PlayerId id, s16 amount);
bool tryAddRupees(PlayerId id, s16 amount);

u16 getArrows(PlayerId id);
u16 getMaxArrows(PlayerId id);
void setMaxArrows(PlayerId id, u16 maxArrows);
bool tryConsumeArrow(PlayerId id);
bool tryAddArrows(PlayerId id, u16 amount);

u8 getPachinko(PlayerId id);
bool tryConsumePachinko(PlayerId id);
bool tryAddPachinko(PlayerId id, u8 amount);

u8 getBombCount(PlayerId id, u8 bagIdx);
bool tryConsumeBomb(PlayerId id, u8 bagIdx);
bool tryAddBombs(PlayerId id, u8 bagIdx, u8 amount);

void syncPlayerFromSave(PlayerId id);
void syncPlayerToSave(PlayerId id);

// Late-join / missing indexed data: use global progression and starter consumables.
void initPlayerFromProgression(PlayerId id);

}  // namespace dusk::coop::inventory
