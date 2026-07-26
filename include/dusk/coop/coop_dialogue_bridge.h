#pragma once

// Thin C-linkage bridge for dialogue pad resolution in co-op.
// Gives the message system a way to determine which legacy pad/device
// should be checked for A/B/Start/skip/choice input during dialogue,
// routing input to the co-op player who owns the active conversation.

#include "dolphin/types.h"

#if TARGET_PC

#ifdef __cplusplus
extern "C" {
#endif

// Returns the legacy pad port (0-3, matching PAD_1..PAD_4) that owns the
// currently-active dialogue or conversation.  Falls back to PAD_1 (0) when:
//   - no co-op dialogue override is active (no active conversation,
//     no talk-style event presentation override)
//   - the owning player has no assigned legacy pad port
//   - the owning player is not joined
//
// For button-triggered TALK conversations: returns the initiator's pad.
// For location-triggered OTHER/message-staff events: returns the resolved
// closest-player's pad (via the presentation override set in demoCheck()).
u8 dusk_coop_resolve_dialogue_pad(void);

// Convenience macro for use in pad-check call sites.
// Under TARGET_PC: expands to the resolved dialogue pad.
// Under non-PC:    expands to 0 (PAD_1, preserving vanilla behavior).
#if TARGET_PC
#define DLPAD() dusk_coop_resolve_dialogue_pad()
#else
#define DLPAD() 0
#endif

#ifdef __cplusplus
}
#endif

#endif  // TARGET_PC