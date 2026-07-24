#pragma once

// Thin C-linkage bridge so dComIfGs_* inlines use indexed player resources
// without pulling the multiplayer runtime into every translation unit.

#include "dolphin/types.h"

#if TARGET_PC

#ifdef __cplusplus
extern "C" {
#endif

// True after indexed resources have been initialized from save data.
int dusk_coop_resourcesReady(void);

u16 dusk_coop_getLife(void);
void dusk_coop_setLife(u16 life);
u16 dusk_coop_getMaxLife(void);
void dusk_coop_setMaxLife(u16 maxLife);

u16 dusk_coop_getRupee(void);
void dusk_coop_setRupee(u16 rupees);

u16 dusk_coop_getOil(void);
void dusk_coop_setOil(u16 oil);
u16 dusk_coop_getMaxOil(void);
void dusk_coop_setMaxOil(u16 maxOil);

u8 dusk_coop_getMagic(void);
void dusk_coop_setMagic(u8 magic);
u8 dusk_coop_getMaxMagic(void);
void dusk_coop_setMaxMagic(u8 maxMagic);

u8 dusk_coop_getArrowNum(void);
void dusk_coop_setArrowNum(u8 num);
u8 dusk_coop_getArrowMax(void);
void dusk_coop_setArrowMax(u8 max);

u8 dusk_coop_getPachinkoNum(void);
void dusk_coop_setPachinkoNum(u8 num);

u8 dusk_coop_getBombNum(u8 bagIdx);
void dusk_coop_setBombNum(u8 bagIdx, u8 num);

u8 dusk_coop_getBottleNum(u8 bottleIdx);
void dusk_coop_setBottleNum(u8 bottleIdx, u8 num);

// Bottle inventory slots SLOT_11..SLOT_14 (idx 0..3). Permanent item slots stay global.
u8 dusk_coop_getBottleItem(u8 bottleIdx);
void dusk_coop_setBottleItem(u8 bottleIdx, u8 itemNo);

#ifdef __cplusplus
}
#endif

#endif  // TARGET_PC
