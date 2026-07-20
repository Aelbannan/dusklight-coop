#pragma once

// Thin C-linkage bridge so dComIfGs_* inlines can route consumables to
// per-player sidecars without pulling the full co-op runtime into every TU.

#include "dolphin/types.h"

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC

#ifdef __cplusplus
extern "C" {
#endif

// True when co-op is compiled in, enabled, and the active context player is not 0.
int dusk_coop_useResourceSidecar(void);

u16 dusk_coop_getLife(void);
void dusk_coop_setLife(u16 life);

u16 dusk_coop_getRupee(void);
void dusk_coop_setRupee(u16 rupees);

u16 dusk_coop_getOil(void);
void dusk_coop_setOil(u16 oil);

u8 dusk_coop_getMagic(void);
void dusk_coop_setMagic(u8 magic);

u8 dusk_coop_getArrowNum(void);
void dusk_coop_setArrowNum(u8 num);

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

#endif  // ENABLE_LOCAL_COOP && TARGET_PC
