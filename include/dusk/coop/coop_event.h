#pragma once

#include "dusk/coop/coop_types.h"
#include "SSystem/SComponent/c_bg_s_poly_info.h"

#include <cstdint>

class daAlink_c;

namespace dusk::coop::event {

// ---------------------------------------------------------------------------
// Event classification
// ---------------------------------------------------------------------------

enum class EventKind : uint8_t {
    None,
    StageEntry,      // stage-start story demo
    StageExit,       // transition via scene exit trigger
    StoryCutscene,   // global story event (party-synchronized)
    PartyStory,      // route-blocking story NPC event
    Conversation,    // initiator-owned NPC conversation
    // deferred kinds for future use
};

enum class EventScope : uint8_t {
    P1Story,
    PartySynchronized,
    InitiatorOwned,
};

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

enum class EventState : uint8_t {
    Idle,
    WaitingForParty,
    ReadyToCommit,
    Running,
    Finishing,
};

// ---------------------------------------------------------------------------
// Target stage descriptor (bounded fixed-size, no heap)
// ---------------------------------------------------------------------------

struct StageTarget {
    char stage[8]{};    // null-terminated stage name
    s8 room = -1;
    s8 point = -1;
    u8 layer = 0xFF;
    s16 wipe = 0;
    f32 wipeSpeed = 0.0f;

    bool operator==(const StageTarget& o) const {
        return room == o.room && point == o.point && layer == o.layer &&
               wipe == o.wipe && wipeSpeed == o.wipeSpeed &&
               __builtin_memcmp(stage, o.stage, sizeof(stage)) == 0;
    }

    bool operator!=(const StageTarget& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------------------
// Event request descriptor
// ---------------------------------------------------------------------------

struct EventRequest {
    EventKind kind = EventKind::None;
    EventScope scope = EventScope::PartySynchronized;
    PlayerId initiator = 0;
    PlayerId presentationOwner = 0;
    cXyz anchor{};
    f32 radius = 450.0f;
    StageTarget target{};
    u16 eventId = 0;
    u32 extraFlags = 0;
};

// ---------------------------------------------------------------------------
// Token representing an active or pending event (index into internal array)
// ---------------------------------------------------------------------------

using EventToken = uint8_t;

constexpr EventToken INVALID_EVENT_TOKEN = 0xFF;
constexpr size_t MAX_PENDING_EVENTS = 4;

// ---------------------------------------------------------------------------
// Talk-style event classification
// ---------------------------------------------------------------------------

// Checks whether an event (identified by its event composit ID from the
// event manager) contains a message-type staff.  Events with a message staff
// are "talk-style" events — they display dialogue text, typically triggered
// by map-tool or location proximity rather than a direct Speak/Talk button.
//
// This classifier is intentionally narrow: it checks for TYPE_MESSAGE staff
// in the event data, NOT broad profile names or event types.  Unrelated
// deferred events (doors, pickups, warps, autonomous cutscenes) that lack a
// message staff return false.
//
// Returns true when the event data is loaded and contains at least one staff
// of type dEvDtStaff_c::TYPE_MESSAGE (staff type 7).  Returns false when the
// event data is not yet available (null), the event ID is -1, or no message
// staff is found.
bool isTalkStyleEvent(s16 eventId);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void init();
void reset();
void tick();

// True while a party-barrier event is still waiting for players to gather.
// Waiting must not collapse split-screen; the single-view presentation rules
// apply only after an event has actually started (or for conversations via
// render::pushConversationPresentation()).
bool hasWaitingForPartyEvent();

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Submit an event request. Returns a token that refers to this event.
// If an equivalent request (same kind + anchor + target) already exists,
// returns the existing token (deduplication).
EventToken request(const EventRequest& req);

// ---------------------------------------------------------------------------
// Throttle interval for waiting notifications (120 frames @ 60fps = 2 s)
// ---------------------------------------------------------------------------

constexpr u32 kToastInterval = 120;

// ---------------------------------------------------------------------------
// Query state
// ---------------------------------------------------------------------------

bool isWaiting(EventToken token);
bool isReady(EventToken token);
bool shouldCommit(EventToken token);
EventState state(EventToken token);
EventRequest getRequest(EventToken token);

// Cancel a pending event. No-op if token is invalid or already committed.
void cancel(EventToken token);

// Force transition to Running (for external commit acknowledgment).
void markRunning(EventToken token);

// Force transition to Finishing -> Idle (for external event-end detection).
void markFinished(EventToken token);

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

// Readiness mask for an active event: bit i is set when player i
// (a) is joined, (b) has a valid Link actor, (c) is inside the
// anchor radius.  A joined player whose actor is temporarily
// unavailable (e.g. still loading) is NOT ready.
uint8_t readinessMask(EventToken token);

// Snapshot of joined players at request time.  Includes every joined
// slot whether or not the Link actor was valid at that instant.
uint8_t participantsMask(EventToken token);

// Returns the number of ready players for the event with the given
// token.  Joined-but-unavailable players count in totalParticipants
// but not in readyCount.
uint8_t readyCount(EventToken token);
uint8_t totalParticipants(EventToken token);

// Returns a human-readable summary of readiness, e.g.
// "Party gathering (2/4 ready)".  The returned buffer is statically
// owned; callers must not free it.
const char* readinessSummary(EventToken token);

// ---------------------------------------------------------------------------
// Deferred stage entry — P1 saves the start demo params during create(),
// the arbiter triggers the actual order when all joined Links are ready.
// ---------------------------------------------------------------------------

// Called by P1's playerInit() to defer the start demo.  Returns a pending
// token that the arbiter will commit once all joined players are near the
// entry anchor, or INVALID_EVENT_TOKEN if no deferral is needed.
EventToken deferStageEntry(int computedEventId, const cXyz& entryAnchor);

// Returns true when the deferred stage entry arbiter has committed.
// Once true, the caller (P1 during its update loop) must call
// commitStageEntryNow() exactly once.
bool isStageEntryReadyToCommit();

// Called exactly once by P1 after isStageEntryReadyToCommit() returns true.
// Returns the saved eventId that was passed to deferStageEntry().
int commitStageEntryNow();

// Returns true while a StageEntry event is pending (waiting or ready).
bool isStageEntryPending();

// ---------------------------------------------------------------------------
// Deferred stage exit — any joined player can request a gameplay exit;
// P1 commits the transition when all joined Links are near the exit.
// ---------------------------------------------------------------------------

struct CapturedExitParams {
    int exitId;
    f32 speed;
    u32 mode;
    s8 roomNo;
    s16 angle;
    int param5;
    bool groundPath;     // true = use dStage_changeSceneExitId path
    cBgS_PolyInfo groundPoly{}; // captured when groundPath is true
    f32 radius{450.0f};         // party gathering radius around anchor
    fpc_ProcID sourceProcId{fpcM_ERROR_PROCESS_ID_e}; // scene-exit actor
    PlayerId initiator{0};
    cXyz initiatorPosition{}; // position when the exit was first triggered
    cXyz anchor{};
};

// Submit a stage-exit event request.  The caller captures the exit params
// BEFORE calling dStage_changeScene/dStage_changeSceneExitId and returns
// false to suppress the transition.  P1 will commit the saved params when
// the arbiter permits it.  Non-exit transitions (menus, logos, deaths,
// peepholes) are not gated — callers must gate only gameplay exit paths.
EventToken deferStageExit(const CapturedExitParams& params);

// Returns true when the deferred stage exit arbiter has committed.
bool isStageExitReadyToCommit();

// Called exactly once by P1 after isStageExitReadyToCommit() returns true.
// Executes the saved dStage_changeScene or dStage_changeSceneExitId call.
void commitStageExitNow();

// Returns the captured exit params for the committed stage exit.
const CapturedExitParams& getCommittedExitParams();

// Returns true while a StageExit event is pending (waiting or ready).
bool isStageExitPending();

// True for the remainder of the frame in which P1 committed the deferred
// stage exit.  This prevents the Link ground-exit path from issuing a second
// vanilla transition after the arbiter has already committed one.
bool wasStageExitCommittedThisFrame();

// Prevent the initiating Link from crossing farther through the exit while
// the party barrier is waiting.  Movement away from the exit remains free.
void enforceStageExitBoundary(fopAc_ac_c* actor, PlayerId player);

// ---------------------------------------------------------------------------
// Party-synchronized story event — a route-blocking NPC event that must be
// witnessed by every joined Link.  The first player to trigger it becomes
// the initiator; P1 is the flow and presentation authority.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Conversation metadata — initiator-owned NPC conversations.  No party
// readiness barrier; the event runs immediately on the global event manager
// while the arbiter tracks initiator attribution and reward distribution.
// ---------------------------------------------------------------------------

struct CapturedConversationParams {
    s16 profName{};        // profile name of the target NPC/actor (fpcNm_*_e)
    u16 eventId{};         // event ID (0 = resolved by event manager)
    char stageName[8]{};   // current stage name for dedup (null-terminated)
    s8 roomNo{-1};         // current room for dedup
    cXyz anchor{};         // NPC position
    PlayerId initiator{0}; // which player triggered the conversation
    fpc_ProcID npcProcId{fpcM_ERROR_PROCESS_ID_e}; // NPC process ID
    u8 mapToolId{0xFF};    // map tool ID (0xFF = none)
    // The request actor used for the vanilla event order:
    // Always P1 for compatibility with setParam/PtT/PtI talk-partner logic.
    // The real initiator (closest Link) is stored in `initiator`.
    fopAc_ac_c* requestActor = nullptr;  // P1 Link, set by arbiter
    fopAc_ac_c* targetActor = nullptr;   // NPC actor pointer
};

struct CapturedPartyStoryParams {
    s16 profName{};        // profile name of the NPC/actor (fpcNm_*_e)
    u16 eventId{};         // the event ID for ordering (0 = unknown, use stage/room dedup fallback)
    u8 mapToolId{0xFF};    // map tool ID (0xFF = none)
    char stageName[8]{};   // current stage name for discriminator (null-terminated)
    s8 roomNo{-1};         // current room for discriminator (-1 = unknown)
    cXyz anchor{};         // NPC position (gathering point)
    PlayerId initiator{0}; // which player triggered the event
    f32 radius{450.0f};    // proximity radius
    fpc_ProcID npcProcId{fpcM_ERROR_PROCESS_ID_e}; // NPC process ID for re-ordering
};

// Submit a party-synchronized story event request.  Returns a token or
// INVALID_EVENT_TOKEN if the classifier rejects this NPC/event.
// The caller (the Speak/Talk event interceptor) must return 0 to suppress
// the vanilla event order; P1 will re-order it when the arbiter commits.
EventToken deferPartyStory(const CapturedPartyStoryParams& params);

// Returns true when the deferred PartyStory arbiter has committed and P1
// should order the vanilla event.
bool isPartyStoryReadyToCommit();

// Result of committing a PartyStory event: params to re-order, plus
// the event token for lifecycle completion tracking.
struct PartyStoryCommitResult {
    CapturedPartyStoryParams params{};
    EventToken token = INVALID_EVENT_TOKEN;
    bool valid = false;
};

// Called exactly once by P1 after isPartyStoryReadyToCommit() returns true.
// Returns the captured PartyStory params and event token.
PartyStoryCommitResult commitPartyStoryNow();

// Returns true while a PartyStory event is pending (waiting or ready).
bool isPartyStoryPending();

// ---------------------------------------------------------------------------
// PartyStory event lifecycle detection — call from P1's update loop to
// detect when the vanilla event has completed.
// ---------------------------------------------------------------------------

// Notify the arbiter that P1 has ordered the vanilla event (transition to
// Running state).
void markPartyStoryRunning(EventToken token);

// Notify the arbiter that the vanilla event has completed (transition to
// Idle).  Returns true if the token was valid.
bool markPartyStoryFinished(EventToken token);

// Track the currently-active PartyStory token for P1 event-end detection.
// The commit code sets this; dEvt_control_c::endProc() reads and clears it.
EventToken getActivePartyStoryToken();
void setActivePartyStoryToken(EventToken token);

// ---------------------------------------------------------------------------
// Conversation API — initiator-owned conversations
// ---------------------------------------------------------------------------

// Submit a conversation metadata record.  Unlike PartyStory, there is no
// readiness barrier — the vanilla event proceeds immediately.  The arbiter
// tracks the initiator (closest Link) and on event-end distributes rewards
// to every joined player.
//
// The caller must still order the vanilla event via dComIfGp_event_order()
// with P1 as the request actor.  This function only tracks metadata.
//
// Returns INVALID_EVENT_TOKEN if a conversation is already active (dedup).
EventToken trackConversation(const CapturedConversationParams& params);

// Returns the active conversation token, or INVALID_EVENT_TOKEN if none.
EventToken getActiveConversationToken();

// Set the active conversation token (used during commit).
void setActiveConversationToken(EventToken token);

// Mark a conversation event as finished.  Called from endProc().
// If the token is valid and the conversation was Running, this will:
//   1. Transition the event slot to Idle
//   2. Call applyConversationRewards() to distribute rewards to all players
// Returns true if the token was processed.
bool markConversationFinished(EventToken token);

// Apply conversation rewards to ALL joined players.
// Currently: the item/resource granted by the vanilla event (read from
// dComIfGp_event_getGtItm()) is distributed to every joined player.
// Global flags are committed once (by the vanilla event) and not duplicated.
void applyConversationRewards(const CapturedConversationParams& params);

}  // namespace dusk::coop::event