#pragma once

// Thin C-linkage bridge for combat attribution hooks in widely-included headers.

#include "dolphin/types.h"

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC

#ifdef __cplusplus
extern "C" {
#endif

// When a non-P0 (or proxy) attack is being attributed, return that cut type;
// otherwise return nativeCutType unchanged.
u8 dusk_coop_overrideCutType(u8 nativeCutType);

#ifdef __cplusplus
}
#endif

#endif  // ENABLE_LOCAL_COOP && TARGET_PC
