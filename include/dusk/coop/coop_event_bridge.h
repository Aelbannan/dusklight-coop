#pragma once

// Thin C-linkage bridge for party-synchronized story event classification
// in game-side code (f_op_actor_mng.cpp, daAlink_c, NPCs).

#include "dolphin/types.h"

#if TARGET_PC

#ifdef __cplusplus
extern "C" {
#endif

// Check whether an NPC interaction (identified by profile name, event ID,
// stage name, and room number) matches a known route-blocking PartyStory
// event.
//
// IMPORTANT: This function MUST discriminate by event + stage + room,
// NOT by broad profile type alone.  Profile types like fpcNm_NPC_KN_e
// (generic Knight NPC) match dozens of ordinary conversations across the
// game.  Classifying every such NPC as PartyStory would incorrectly gate
// all mundane interactions behind the party barrier.
//
// Returns nonzero (true) if this specific event+stage+room combination
// should be gated by the event arbiter.
//
// Currently recognized route-blocking event contexts:
//   (none yet — first discovery via debug logs must provide a concrete
//    eventId + stageName + roomNo combination)
//
// When a new context is confirmed, add it to the implementation in
// coop_event.cpp.  Until then, this function always returns 0 and logs
// every candidate for discovery.
u8 dusk_coop_isPartyStoryEvent(s16 profName, u16 eventId,
                                const char* stageName, s8 roomNo);

// Log a potential PartyStory candidate.  Calling this from the
// orderSpeakEvent / orderTalkEvent interceptors helps identify new
// route-blocking events during runtime discovery.  Reports whether
// the candidate matched (isPartyStoryEvent) or not.
void dusk_coop_logPartyStoryCandidate(s16 profName, u16 eventId,
                                       const char* stageName, s8 roomNo);

#ifdef __cplusplus
}
#endif

#endif  // TARGET_PC