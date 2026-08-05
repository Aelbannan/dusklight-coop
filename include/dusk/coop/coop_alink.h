#pragma once

#include "dusk/coop/coop_types.h"

class daAlink_c;

namespace dusk::coop::alink {

// Every player owns a real daAlink_c and indexed create state.

void registerPendingSpawn(PlayerId player);
void noteSpawnProcess(PlayerId player, u32 processId);
void clearSpawn(PlayerId player);
void clearPendingSpawn();
void markSpawnComplete(PlayerId player);

// Resolve which indexed player slot owns this Link.
PlayerId resolveOwner(const daAlink_c* link);

// Per-player create wait flag for every engine slot.
BOOL& bgWaitFlag(PlayerId player);

PlayerId ownerOf(const daAlink_c* link);
bool isStoryAuthorityLink(const daAlink_c* link);

// Called whenever an indexed Link finishes creating.
void onLinkCreated(PlayerId id, daAlink_c* link);
void clearLinkOwner(PlayerId id, const daAlink_c* link);

// Fill stick/buttons from PlayerInputSnapshot. Returns true if applied.
bool applyInputSnapshot(daAlink_c* link);

// ---------------------------------------------------------------------------
// Location-based dialogue trigger helper
// ---------------------------------------------------------------------------

// Parameters for per-player dialogue eligibility checks.
struct DialogueTriggerParams {
    fopAc_ac_c* tagActor;      // the tag actor (for angle calculations)
    cXyz center;               // trigger center (tag actor position)
    f32 radiusXZ;              // horizontal range (typically scale.x)
    f32 halfHeightY;           // vertical half-extent (typically scale.y)
    s16 facingArc;             // max allowed angle difference from player facing
                               // to the tag (e.g. 0x1000). Set to 0 to skip
                               // the facing check.
    bool needFacingCheck;      // true = apply the facing arc test
};

// For location-based dialogue triggers: iterate all joined co-op players
// whose Link actor is valid, check that the player is within the XZ radius
// and Y half-height of the trigger center, and optionally check the facing
// angle.  Selects the closest eligible player (by XZ distance).
//
// Returns the closest eligible PlayerId, or DIALOGUE_PLAYER_NONE (0xFF)
// when no joined player satisfies the conditions.
//
// The caller is responsible for ordering a single event with the returned
// player's context.  P1 remains the vanilla request actor for flow
// compatibility.
PlayerId resolveClosestDialoguePlayer(const DialogueTriggerParams& params);

// Select the nearest joined Link to an actor position. Used while executing
// NPC interaction logic so legacy NPC code that reads the vanilla player
// accessor evaluates against the player closest to that NPC.
PlayerId resolveNearestPlayer(const cXyz& center);

// Returns and clears the player selected by the most recent trigger check for
// |tagActor|.  This bridges a tag's eligibility check to the immediately
// following vanilla event-order call without relying on the ambient context.
PlayerId consumeDialogueTriggerPlayer(fopAc_ac_c* tagActor);
void rememberDialogueTriggerPlayer(fopAc_ac_c* tagActor, PlayerId player);
void clearDialogueTriggerPlayer();

// Sentinel returned by resolveClosestDialoguePlayer when no joined player
// satisfies the trigger conditions.
constexpr PlayerId DIALOGUE_PLAYER_NONE = 0xFF;

}  // namespace dusk::coop::alink
