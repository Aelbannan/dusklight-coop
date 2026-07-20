#pragma once

// Thin C-linkage bridge so player / save / kankyo headers can route wolf and
// transform queries without pulling the full co-op runtime into every TU.

#include "dolphin/types.h"

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC

#ifdef __cplusplus
extern "C" {
#endif

int dusk_coop_checkNowWolf(void);
int dusk_coop_checkNowWolfAuthority(void);
int dusk_coop_checkNowWolfEyeUp(void);
int dusk_coop_trySetTransformStatus(u8 status);
u8 dusk_coop_getTransformStatusForQuery(void);
int dusk_coop_sensesActiveForCurrentView(void);

#ifdef __cplusplus
}
#endif

#endif  // ENABLE_LOCAL_COOP && TARGET_PC
