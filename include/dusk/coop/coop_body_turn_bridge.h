#pragma once

// Co-op body-turn resolution for NPC dialogue-facing.
// During an active co-op conversation presentation, the NPC body-turn
// target is the presentation owner's Link instead of P1.
// The head/eyes already track the correct player; this fixes the body.

#include "dolphin/types.h"

#if TARGET_PC

#include "f_op/f_op_actor_mng.h"  // fopAcM_searchPlayerAngleY, fopAc_ac_c

#ifdef __cplusplus
extern "C" {
#endif

// --- C-linkage bridge functions (called from co-op subsystem) -----------

// Returns the angle from |npc| to the presentation owner's Link during
// an active co-op conversation presentation.  Falls back to
// fopAcM_searchPlayerAngleY(npc) when no presentation is active.
s16 dusk_coop_resolve_body_turn_angle(fopAc_ac_c* npc);

// Returns the presentation owner's Link actor pointer during an active
// co-op conversation presentation.  Falls back to daPy_getPlayerActorClass()
// when no presentation is active.
fopAc_ac_c* dusk_coop_resolve_body_turn_actor(void);

// Returns true when |curAngleY| equals the presentation owner's angle,
// or fopAcM_searchPlayerAngleY(npc) when no presentation is active.
// Equivalent to the vanilla pattern:  curAngleY == fopAcM_searchPlayerAngleY(npc)
BOOL dusk_coop_body_turn_angle_equals(fopAc_ac_c* npc, s16 curAngleY);

#ifdef __cplusplus
}
#endif

// --- Convenience macros for NPC talk-action call sites ----------------
// Under TARGET_PC:  resolve via the co-op bridge.
// Under non-PC:     expand to vanilla P1.

#define BODY_TURN_ANGLE(npc)    dusk_coop_resolve_body_turn_angle(npc)
#define BODY_TURN_ACTOR()       dusk_coop_resolve_body_turn_actor()
#define BODY_TURN_EQUALS(npc, curAngleY)  dusk_coop_body_turn_angle_equals(npc, curAngleY)

#else  // !TARGET_PC

#define BODY_TURN_ANGLE(npc)    fopAcM_searchPlayerAngleY(npc)
#define BODY_TURN_ACTOR()       daPy_getPlayerActorClass()
#define BODY_TURN_EQUALS(npc, curAngleY)  ((curAngleY) == fopAcM_searchPlayerAngleY(npc))

#endif  // TARGET_PC