#include "dusk/coop/coop_event.h"
#include "dusk/coop/coop_event_bridge.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_accessors.h"
#include "dusk/coop/coop_alink.h"
#include "dusk/coop/coop_bottles.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_inventory.h"
#include "dusk/coop/coop_render.h"
#include "dusk/ui/ui.hpp"

#if TARGET_PC
#include "d/d_com_inf_game.h"
#include "d/d_item.h"
#include "d/d_item_data.h"
#include "d/d_stage.h"
#include "f_pc/f_pc_name.h"
#endif

#include <cmath>
#include <cstdio>

namespace dusk::coop::event {
namespace {

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

struct EventSlot {
    bool active = false;
    EventRequest request{};
    EventState state = EventState::Idle;
    uint8_t readinessMask = 0;
    uint8_t participantsMask = 0;
    u32 tickCounter = 0;
    u32 lastToastTick = 0;
    uint8_t lastReadinessForToast = 0;
};

static std::array<EventSlot, MAX_PENDING_EVENTS> g_events{};

// Active PartyStory token — must be before any function that references it.
static EventToken s_activePartyStoryToken = INVALID_EVENT_TOKEN;
static bool s_stageExitCommittedThisFrame = false;

// ---------------------------------------------------------------------------
// Active conversation tracking (no party barrier; immediate event)
// ---------------------------------------------------------------------------

static EventToken s_activeConversationToken = INVALID_EVENT_TOKEN;
static CapturedConversationParams s_activeConversationParams{};

// ---------------------------------------------------------------------------
// Deferred stage entry state
// ---------------------------------------------------------------------------

static struct {
    bool hasPending = false;
    bool committed = false;
    int savedEventId = 0xFF;
    cXyz entryAnchor{};
    EventToken token = INVALID_EVENT_TOKEN;
} g_deferredEntry{};

// ---------------------------------------------------------------------------
// Deferred stage exit state
// ---------------------------------------------------------------------------

static struct {
    bool hasPending = false;
    bool committed = false;
    CapturedExitParams captured{};
    EventToken token = INVALID_EVENT_TOKEN;
} g_deferredExit{};

// ---------------------------------------------------------------------------
// Deferred party-synchronized story event state
// ---------------------------------------------------------------------------

static struct {
    bool hasPending = false;
    bool committed = false;
    CapturedPartyStoryParams captured{};
    EventToken token = INVALID_EVENT_TOKEN;
} g_deferredPartyStory{};

// Conversation metadata (no party barrier, immediate event)
// ---------------------------------------------------------------------------
// No deferred state needed — conversations are tracked via the active token
// and params above; they skip WaitingForParty/ReadyToCommit and go straight
// to Running.

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const char* stateName(EventState s) {
    switch (s) {
    case EventState::Idle:            return "Idle";
    case EventState::WaitingForParty: return "WaitingForParty";
    case EventState::ReadyToCommit:   return "ReadyToCommit";
    case EventState::Running:         return "Running";
    case EventState::Finishing:       return "Finishing";
    }
    return "?";
}

const char* kindName(EventKind k) {
    switch (k) {
    case EventKind::None:          return "None";
    case EventKind::StageEntry:    return "StageEntry";
    case EventKind::StageExit:     return "StageExit";
    case EventKind::StoryCutscene: return "StoryCutscene";
    case EventKind::PartyStory:    return "PartyStory";
    case EventKind::Conversation:  return "Conversation";
    }
    return "?";
}

const char* scopeName(EventScope s) {
    switch (s) {
    case EventScope::P1Story:           return "P1Story";
    case EventScope::PartySynchronized: return "PartySynchronized";
    case EventScope::InitiatorOwned:    return "InitiatorOwned";
    }
    return "?";
}

EventToken allocSlot() {
    for (size_t i = 0; i < MAX_PENDING_EVENTS; ++i) {
        if (!g_events[i].active) {
            return static_cast<EventToken>(i);
        }
    }
    return INVALID_EVENT_TOKEN;
}

EventSlot* slotFor(EventToken t) {
    if (t >= MAX_PENDING_EVENTS) {
        return nullptr;
    }
    if (!g_events[t].active) {
        return nullptr;
    }
    return &g_events[t];
}

bool isLinkActorValid(PlayerId id) {
    return isJoined(id) && getPlayerActor(id) != nullptr;
}

bool anchorsEqual(const cXyz& a, const cXyz& b, f32 eps = 10.0f) {
    return std::fabs(a.x - b.x) < eps &&
           std::fabs(a.y - b.y) < eps &&
           std::fabs(a.z - b.z) < eps;
}

bool isEquivalent(const EventRequest& a, const EventRequest& b) {
    if (a.kind != b.kind) return false;
    if (a.kind == EventKind::None) return false;
    if (a.target != b.target) return false;
    if (a.eventId != b.eventId) return false;
    if (a.kind == EventKind::StageExit ||
        a.kind == EventKind::PartyStory ||
        a.kind == EventKind::Conversation) {
        if (!anchorsEqual(a.anchor, b.anchor)) return false;
    }
    // NOTE: npcProcId is NOT part of EventRequest (it lives in
    // CapturedConversationParams).  Conversations at the same anchor
    // with different NPCs are deduplicated by anchor + eventId + stage;
    // this is conservative but avoids struct bloat.  If false dedup
    // is observed in practice, add npcProcId to EventRequest or
    // increase the anchor equality epsilon.
    return true;
}

void recomputeReadiness(EventSlot& slot) {
    if (!slot.active) {
        slot.readinessMask = 0;
        return;
    }
    uint8_t mask = 0;
    for (uint8_t i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (!(slot.participantsMask & (1u << i))) continue;
        if (!isLinkActorValid(i)) continue;
        fopAc_ac_c* actor = getPlayerActor(i);
        const f32 dist = slot.request.anchor.absXZ(actor->current.pos);
        if (dist <= slot.request.radius) {
            mask |= (1u << i);
        }
    }
    slot.readinessMask = mask;
}

void logTransition(EventToken token, const EventSlot& slot,
                   EventState from, EventState to,
                   const char* note = "") {
    debug::logInfo(
        "event token=%u kind=%s scope=%s state=%s->%s initiator=%u "
        "participants=0x%02x readiness=0x%02x radius=%.1f "
        "target=%.4s/room=%d eventId=%u%s%s",
        static_cast<unsigned>(token),
        kindName(slot.request.kind),
        scopeName(slot.request.scope),
        stateName(from),
        stateName(to),
        static_cast<unsigned>(slot.request.initiator),
        static_cast<unsigned>(slot.participantsMask),
        static_cast<unsigned>(slot.readinessMask),
        static_cast<double>(slot.request.radius),
        slot.request.target.stage,
        static_cast<int>(slot.request.target.room),
        static_cast<unsigned>(slot.request.eventId),
        (note[0] ? " " : ""),
        note);
}

bool allReady(const EventSlot& slot) {
    return slot.participantsMask != 0 &&
           (slot.readinessMask & slot.participantsMask) == slot.participantsMask;
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void init() {
    for (auto& slot : g_events) {
        slot = {};
    }
    g_deferredEntry = {};
    g_deferredExit = {};
    g_deferredPartyStory = {};
    s_activePartyStoryToken = INVALID_EVENT_TOKEN;
    s_stageExitCommittedThisFrame = false;
    s_activeConversationToken = INVALID_EVENT_TOKEN;
    s_activeConversationParams = {};
    debug::logInfo("event: init");
}

void reset() {
    // Pop any active presentation override before clearing event state.
    render::popConversationPresentation();

    EventState oldStates[MAX_PENDING_EVENTS];
    for (size_t i = 0; i < MAX_PENDING_EVENTS; ++i) {
        oldStates[i] = g_events[i].state;
    }
    init();
    for (size_t i = 0; i < MAX_PENDING_EVENTS; ++i) {
        if (oldStates[i] != EventState::Idle) {
            debug::logInfo("event token=%zu state=%s->Idle (reset)", i,
                           stateName(oldStates[i]));
        }
    }
}

bool hasWaitingForPartyEvent() {
    for (const EventSlot& slot : g_events) {
        if (slot.active && slot.state == EventState::WaitingForParty) {
            return true;
        }
    }
    return false;
}

void tick() {
    // Keep this flag visible for the remainder of the frame in which
    // commitStageExitNow() runs; clear it before the next simulation tick.
    s_stageExitCommittedThisFrame = false;

    for (EventToken token = 0; token < MAX_PENDING_EVENTS; ++token) {
        EventSlot& slot = g_events[token];
        if (!slot.active) continue;

        ++slot.tickCounter;
        const EventState prevState = slot.state;
        const uint8_t prevReadiness = slot.readinessMask;

        switch (slot.state) {

        case EventState::Idle:
            slot.active = false;
            break;

        case EventState::WaitingForParty:
            recomputeReadiness(slot);
            if (allReady(slot)) {
                slot.state = EventState::ReadyToCommit;
                logTransition(token, slot, prevState, slot.state);
            } else {
                if (slot.readinessMask != prevReadiness) {
                    slot.lastToastTick = slot.tickCounter;
                    slot.lastReadinessForToast = slot.readinessMask;
                    debug::logInfo(
                        "event token=%u kind=%s readiness changed 0x%02x->0x%02x "
                        "(%u ready/%u participants)",
                        static_cast<unsigned>(token),
                        kindName(slot.request.kind),
                        static_cast<unsigned>(prevReadiness),
                        static_cast<unsigned>(slot.readinessMask),
                        static_cast<unsigned>(__builtin_popcount(slot.readinessMask)),
                        static_cast<unsigned>(__builtin_popcount(slot.participantsMask)));
                }
                // Notify when the readiness set changes.  Do not replay the
                // toast indefinitely while the same player remains away.
                if (slot.readinessMask != prevReadiness) {
                    dusk::ui::push_toast_to_all_views({
                        .type = "warning",
                        .title = "Gather Up",
                        .content = readinessSummary(token),
                        .duration = std::chrono::seconds(4),
                    });
                    slot.lastToastTick = slot.tickCounter;
                    slot.lastReadinessForToast = slot.readinessMask;
                }
            }
            break;

        case EventState::ReadyToCommit:
            recomputeReadiness(slot);
            if (!allReady(slot)) {
                slot.state = EventState::WaitingForParty;
                logTransition(token, slot, prevState, slot.state, "regressed");
            } else {
                // Auto-commit stage entry when all joined players are ready.
                if (slot.request.kind == EventKind::StageEntry) {
                    if (g_deferredEntry.hasPending && !g_deferredEntry.committed) {
                        g_deferredEntry.committed = true;
                        slot.state = EventState::Running;
                        logTransition(token, slot, prevState, slot.state,
                                      "stage-entry auto-commit");
                        debug::logInfo(
                            "event: stage entry committed token=%u eventId=%d",
                            static_cast<unsigned>(token),
                            g_deferredEntry.savedEventId);
                    }
                }
                // Auto-commit stage exit when all joined players are ready.
                if (slot.request.kind == EventKind::StageExit) {
                    if (g_deferredExit.hasPending && !g_deferredExit.committed) {
                        g_deferredExit.committed = true;
                        slot.state = EventState::Running;
                        logTransition(token, slot, prevState, slot.state,
                                      "stage-exit auto-commit");
                        debug::logInfo(
                            "event: stage exit committed token=%u exitId=%d",
                            static_cast<unsigned>(token),
                            g_deferredExit.captured.exitId);
                    }
                }
                // Auto-commit party story event when all joined players are ready.
                if (slot.request.kind == EventKind::PartyStory) {
                    if (g_deferredPartyStory.hasPending && !g_deferredPartyStory.committed) {
                        g_deferredPartyStory.committed = true;
                        slot.state = EventState::Running;
                        logTransition(token, slot, prevState, slot.state,
                                      "partystory auto-commit");
                        debug::logInfo(
                            "event: party story committed token=%u profName=0x%04x "
                            "eventId=%u stage=%.4s room=%d",
                            static_cast<unsigned>(token),
                            static_cast<unsigned>(g_deferredPartyStory.captured.profName),
                            static_cast<unsigned>(g_deferredPartyStory.captured.eventId),
                            g_deferredPartyStory.captured.stageName,
                            static_cast<int>(g_deferredPartyStory.captured.roomNo));
                    }
                }
                // Auto-commit conversation immediately (no party barrier).
                // Conversations skip WaitingForParty entirely and are created
                // with state=ReadyToCommit, then auto-committed here.
                if (slot.request.kind == EventKind::Conversation) {
                    if (s_activeConversationToken != INVALID_EVENT_TOKEN &&
                        slot.state == EventState::ReadyToCommit)
                    {
                        // Already marked Running on trackConversation().
                    } else if (slot.state == EventState::ReadyToCommit) {
                        slot.state = EventState::Running;
                        logTransition(token, slot, prevState, slot.state,
                                      "conversation auto-commit");
                    }
                }
            }
            break;

        case EventState::Running:
        case EventState::Finishing:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

EventToken request(const EventRequest& req) {
    if (req.initiator >= MAX_LOCAL_PLAYERS ||
        req.presentationOwner >= MAX_LOCAL_PLAYERS) {
        debug::logWarn("event: request with out-of-bounds initiator=%u or owner=%u",
                       static_cast<unsigned>(req.initiator),
                       static_cast<unsigned>(req.presentationOwner));
        return INVALID_EVENT_TOKEN;
    }

    if (req.kind == EventKind::None) {
        debug::logWarn("event: request with kind=None rejected");
        return INVALID_EVENT_TOKEN;
    }

    for (EventToken i = 0; i < MAX_PENDING_EVENTS; ++i) {
        if (!g_events[i].active) continue;
        if (isEquivalent(g_events[i].request, req)) {
            debug::logInfo(
                "event: dedup token=%u kind=%s target=%.4s/room=%d eventId=%u",
                static_cast<unsigned>(i),
                kindName(req.kind),
                req.target.stage,
                static_cast<int>(req.target.room),
                static_cast<unsigned>(req.eventId));
            return i;
        }
    }

    const EventToken token = allocSlot();
    if (token == INVALID_EVENT_TOKEN) {
        debug::logError("event: no free slots (max %zu)", MAX_PENDING_EVENTS);
        return INVALID_EVENT_TOKEN;
    }

    EventSlot& slot = g_events[token];
    slot.active = true;
    slot.request = req;
    slot.state = EventState::WaitingForParty;
    slot.readinessMask = 0;
    slot.tickCounter = 0;

    slot.participantsMask = 0;
    for (uint8_t i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (isJoined(i)) {
            slot.participantsMask |= (1u << i);
        }
    }

    recomputeReadiness(slot);

    if (!allReady(slot)) {
        dusk::ui::push_toast_to_all_views({
            .type = "warning",
            .title = "Gather Up",
            .content = readinessSummary(token),
            .duration = std::chrono::seconds(4),
        });
        slot.lastReadinessForToast = slot.readinessMask;
    }

    debug::logInfo(
        "event: request token=%u kind=%s scope=%s initiator=%u "
        "owner=%u participants=0x%02x radius=%.1f "
        "target=%.4s/room=%d/point=%d/layer=%u/wipe=%d "
        "eventId=%u anchor=(%.0f,%.0f,%.0f)",
        static_cast<unsigned>(token),
        kindName(req.kind),
        scopeName(req.scope),
        static_cast<unsigned>(req.initiator),
        static_cast<unsigned>(req.presentationOwner),
        static_cast<unsigned>(slot.participantsMask),
        static_cast<double>(req.radius),
        req.target.stage,
        static_cast<int>(req.target.room),
        static_cast<int>(req.target.point),
        static_cast<unsigned>(req.target.layer),
        static_cast<int>(req.target.wipe),
        static_cast<unsigned>(req.eventId),
        static_cast<double>(req.anchor.x),
        static_cast<double>(req.anchor.y),
        static_cast<double>(req.anchor.z));

    return token;
}

bool isWaiting(EventToken token) {
    const EventSlot* slot = slotFor(token);
    return slot != nullptr && slot->state == EventState::WaitingForParty;
}

bool isReady(EventToken token) {
    const EventSlot* slot = slotFor(token);
    return slot != nullptr && slot->state == EventState::ReadyToCommit;
}

bool shouldCommit(EventToken token) {
    const EventSlot* slot = slotFor(token);
    return slot != nullptr && slot->state == EventState::ReadyToCommit;
}

EventState state(EventToken token) {
    const EventSlot* slot = slotFor(token);
    return slot != nullptr ? slot->state : EventState::Idle;
}

EventRequest getRequest(EventToken token) {
    const EventSlot* slot = slotFor(token);
    return slot != nullptr ? slot->request : EventRequest{};
}

void cancel(EventToken token) {
    EventSlot* slot = slotFor(token);
    if (slot == nullptr) return;
    const EventState oldState = slot->state;
    // If cancelling the tracked PartyStory event, clear the active token.
    if (s_activePartyStoryToken == token) {
        s_activePartyStoryToken = INVALID_EVENT_TOKEN;
    }
    // If cancelling the tracked conversation, pop the presentation override
    // and clear the active token.
    if (s_activeConversationToken == token) {
        render::popConversationPresentation();
        s_activeConversationToken = INVALID_EVENT_TOKEN;
        s_activeConversationParams = {};
    }
    // Clear any deferred state associated with this token.
    if (g_deferredEntry.token == token) {
        g_deferredEntry = {};
    }
    if (g_deferredExit.token == token) {
        g_deferredExit = {};
    }
    if (g_deferredPartyStory.token == token) {
        g_deferredPartyStory = {};
    }
    slot->active = false;
    slot->state = EventState::Idle;
    debug::logInfo("event: cancel token=%u state=%s->Idle",
                   static_cast<unsigned>(token), stateName(oldState));
}

void markRunning(EventToken token) {
    EventSlot* slot = slotFor(token);
    if (slot == nullptr) return;
    if (slot->state != EventState::ReadyToCommit) return;
    const EventState oldState = slot->state;
    slot->state = EventState::Running;
    logTransition(token, *slot, oldState, slot->state, "external commit");
}

void markFinished(EventToken token) {
    EventSlot* slot = slotFor(token);
    if (slot == nullptr) return;
    const EventState oldState = slot->state;
    if (oldState == EventState::Running) {
        // If this is the tracked PartyStory event, clear the active token
        // so event-end cleanup doesn't reference a stale slot.
        if (s_activePartyStoryToken == token) {
            s_activePartyStoryToken = INVALID_EVENT_TOKEN;
        }
        slot->state = EventState::Finishing;
        logTransition(token, *slot, oldState, slot->state);
        slot->active = false;
        slot->state = EventState::Idle;
        debug::logInfo("event: finish token=%u -> Idle (reclaimed)",
                       static_cast<unsigned>(token));
    }
}

bool markPartyStoryFinished(EventToken token) {
    // Validate that this is the currently-active PartyStory event.
    if (s_activePartyStoryToken != token) {
        debug::logWarn(
            "event: markPartyStoryFinished token=%u != active=%u "
            "— ignoring stale finish",
            static_cast<unsigned>(token),
            static_cast<unsigned>(s_activePartyStoryToken));
        if (s_activePartyStoryToken != INVALID_EVENT_TOKEN) {
            // The active token is stale (slot already reclaimed). Clear it.
            s_activePartyStoryToken = INVALID_EVENT_TOKEN;
        }
        return false;
    }
    EventSlot* slot = slotFor(token);
    if (slot == nullptr) {
        // Slot already reclaimed; clear the stale active token.
        s_activePartyStoryToken = INVALID_EVENT_TOKEN;
        return false;
    }
    const EventState oldState = slot->state;
    if (oldState != EventState::Running) {
        debug::logWarn(
            "event: markPartyStoryFinished token=%u state=%s != Running"
            " — ignoring invalid transition",
            static_cast<unsigned>(token),
            stateName(oldState));
        return false;
    }
    slot->state = EventState::Finishing;
    logTransition(token, *slot, oldState, slot->state);
    slot->active = false;
    slot->state = EventState::Idle;
    s_activePartyStoryToken = INVALID_EVENT_TOKEN;
    debug::logInfo("event: party story finish token=%u -> Idle (reclaimed)",
                   static_cast<unsigned>(token));
    return true;
}

uint8_t readinessMask(EventToken token) {
    const EventSlot* slot = slotFor(token);
    return slot != nullptr ? slot->readinessMask : 0;
}

uint8_t participantsMask(EventToken token) {
    const EventSlot* slot = slotFor(token);
    return slot != nullptr ? slot->participantsMask : 0;
}

uint8_t readyCount(EventToken token) {
    const EventSlot* slot = slotFor(token);
    if (slot == nullptr) return 0;
    return static_cast<uint8_t>(__builtin_popcount(slot->readinessMask));
}

uint8_t totalParticipants(EventToken token) {
    const EventSlot* slot = slotFor(token);
    if (slot == nullptr) return 0;
    return static_cast<uint8_t>(__builtin_popcount(slot->participantsMask));
}

const char* readinessSummary(EventToken token) {
    static char s_buf[64];
    const EventSlot* slot = slotFor(token);
    if (slot == nullptr) {
        s_buf[0] = '\0';
        return s_buf;
    }
    const unsigned ready = static_cast<unsigned>(__builtin_popcount(slot->readinessMask));
    const unsigned total = static_cast<unsigned>(__builtin_popcount(slot->participantsMask));
    s_buf[sizeof(s_buf) - 1] = '\0';
    snprintf(s_buf, sizeof(s_buf) - 1, "Party gathering (%u/%u ready)", ready, total);
    return s_buf;
}

// ---------------------------------------------------------------------------
// Deferred stage entry API
// ---------------------------------------------------------------------------

EventToken deferStageEntry(int computedEventId, const cXyz& entryAnchor) {
    if (g_deferredEntry.hasPending) {
        debug::logInfo("event: deferStageEntry already pending token=%u",
                       static_cast<unsigned>(g_deferredEntry.token));
        return g_deferredEntry.token;
    }

    g_deferredEntry = {};
    g_deferredEntry.hasPending = true;
    g_deferredEntry.savedEventId = computedEventId;
    g_deferredEntry.entryAnchor = entryAnchor;

    EventRequest req{};
    req.kind = EventKind::StageEntry;
    req.scope = EventScope::P1Story;
    req.initiator = 0;
    req.presentationOwner = 0;
    req.anchor = entryAnchor;
    req.radius = 600.0f;
    req.eventId = static_cast<u16>(computedEventId >= 0 ? computedEventId : 0);

    const EventToken token = request(req);
    g_deferredEntry.token = token;

    debug::logInfo(
        "event: deferStageEntry eventId=%d token=%u anchor=(%.0f,%.0f,%.0f)",
        computedEventId, static_cast<unsigned>(token),
        static_cast<double>(entryAnchor.x),
        static_cast<double>(entryAnchor.y),
        static_cast<double>(entryAnchor.z));

    return token;
}

bool isStageEntryReadyToCommit() {
    return g_deferredEntry.hasPending && g_deferredEntry.committed;
}

int commitStageEntryNow() {
    if (!g_deferredEntry.hasPending || !g_deferredEntry.committed) {
        return 0xFF;
    }
    const int eventId = g_deferredEntry.savedEventId;
    g_deferredEntry = {};
    debug::logInfo("event: commitStageEntryNow eventId=%d", eventId);
    return eventId;
}

bool isStageEntryPending() {
    return g_deferredEntry.hasPending && !g_deferredEntry.committed;
}

// ---------------------------------------------------------------------------
// Deferred party story API
// ---------------------------------------------------------------------------

EventToken deferPartyStory(const CapturedPartyStoryParams& params) {
    if (g_deferredPartyStory.hasPending) {
        // Dedup: same NPC profile name + same event ID + same stage/room = same request.
        const bool sameEvent = g_deferredPartyStory.captured.eventId == params.eventId ||
                               (g_deferredPartyStory.captured.eventId == 0 && params.eventId == 0);
        const bool sameStage = sameEvent &&
                               g_deferredPartyStory.captured.roomNo == params.roomNo &&
                               __builtin_memcmp(g_deferredPartyStory.captured.stageName,
                                                params.stageName,
                                                sizeof(g_deferredPartyStory.captured.stageName)) == 0;
        if (g_deferredPartyStory.captured.profName == params.profName &&
            g_deferredPartyStory.captured.mapToolId == params.mapToolId &&
            sameStage)
        {
            debug::logInfo(
                "event: deferPartyStory dedup profName=0x%04x eventId=%u "
                "stage=%.4s room=%d token=%u",
                static_cast<unsigned>(params.profName),
                static_cast<unsigned>(params.eventId),
                params.stageName,
                static_cast<int>(params.roomNo),
                static_cast<unsigned>(g_deferredPartyStory.token));
            return g_deferredPartyStory.token;
        }
        debug::logInfo(
            "event: deferPartyStory replacing prev profName=0x%04x/ev=%u/stage=%.4s "
            "with profName=0x%04x/ev=%u/stage=%.4s",
            static_cast<unsigned>(g_deferredPartyStory.captured.profName),
            static_cast<unsigned>(g_deferredPartyStory.captured.eventId),
            g_deferredPartyStory.captured.stageName,
            static_cast<unsigned>(params.profName),
            static_cast<unsigned>(params.eventId),
            params.stageName);
        if (g_deferredPartyStory.token != INVALID_EVENT_TOKEN) {
            cancel(g_deferredPartyStory.token);
        }
        g_deferredPartyStory = {};
    }

    g_deferredPartyStory.hasPending = true;
    g_deferredPartyStory.captured = params;

    EventRequest req{};
    req.kind = EventKind::PartyStory;
    req.scope = EventScope::PartySynchronized;
    req.initiator = params.initiator;
    req.presentationOwner = 0;  // P1 is presentation focus
    req.anchor = params.anchor;
    req.radius = params.radius;
    req.eventId = params.eventId;
    req.extraFlags = static_cast<u32>(params.mapToolId);

    const EventToken token = request(req);
    g_deferredPartyStory.token = token;

    debug::logInfo(
        "event: deferPartyStory profName=0x%04x eventId=%u initiator=%u "
        "token=%u stage=%.4s room=%d anchor=(%.0f,%.0f,%.0f)",
        static_cast<unsigned>(params.profName),
        static_cast<unsigned>(params.eventId),
        static_cast<unsigned>(params.initiator),
        static_cast<unsigned>(token),
        params.stageName,
        static_cast<int>(params.roomNo),
        static_cast<double>(params.anchor.x),
        static_cast<double>(params.anchor.y),
        static_cast<double>(params.anchor.z));

    return token;
}

bool isPartyStoryReadyToCommit() {
    return g_deferredPartyStory.hasPending && g_deferredPartyStory.committed;
}

PartyStoryCommitResult commitPartyStoryNow() {
    PartyStoryCommitResult result{};
    if (!g_deferredPartyStory.hasPending || !g_deferredPartyStory.committed) {
        return result;
    }
    result.params = g_deferredPartyStory.captured;
    result.token = g_deferredPartyStory.token;
    result.valid = true;
    g_deferredPartyStory = {};
    debug::logInfo(
        "event: commitPartyStoryNow profName=0x%04x eventId=%u token=%u "
        "stage=%.4s room=%d",
        static_cast<unsigned>(result.params.profName),
        static_cast<unsigned>(result.params.eventId),
        static_cast<unsigned>(result.token),
        result.params.stageName,
        static_cast<int>(result.params.roomNo));
    return result;
}

bool isPartyStoryPending() {
    return g_deferredPartyStory.hasPending && !g_deferredPartyStory.committed;
}

void markPartyStoryRunning(EventToken token) {
    EventSlot* slot = slotFor(token);
    if (slot == nullptr) return;
    const EventState oldState = slot->state;
    if (oldState == EventState::ReadyToCommit || oldState == EventState::WaitingForParty) {
        slot->state = EventState::Running;
        logTransition(token, *slot, oldState, slot->state, "p1-ordered");
    }
}

// ---------------------------------------------------------------------------
// Deferred stage exit API
// ---------------------------------------------------------------------------

EventToken deferStageExit(const CapturedExitParams& params) {
    if (g_deferredExit.hasPending) {
        if (g_deferredExit.captured.exitId == params.exitId &&
            g_deferredExit.captured.roomNo == params.roomNo &&
            g_deferredExit.captured.groundPath == params.groundPath) {
            debug::logInfo("event: deferStageExit dedup exitId=%d token=%u",
                           params.exitId,
                           static_cast<unsigned>(g_deferredExit.token));
            return g_deferredExit.token;
        }
        debug::logInfo("event: deferStageExit replacing prev exitId=%d with %d",
                       g_deferredExit.captured.exitId, params.exitId);
        if (g_deferredExit.token != INVALID_EVENT_TOKEN) {
            cancel(g_deferredExit.token);
        }
        g_deferredExit = {};
    }

    g_deferredExit.hasPending = true;
    g_deferredExit.captured = params;

    EventRequest req{};
    req.kind = EventKind::StageExit;
    req.scope = EventScope::PartySynchronized;
    req.initiator = 0;
    req.presentationOwner = 0;
    req.anchor = params.anchor;
    req.radius = 450.0f;

    const EventToken token = request(req);
    g_deferredExit.token = token;

    debug::logInfo(
        "event: deferStageExit exitId=%d token=%u anchor=(%.0f,%.0f,%.0f)",
        params.exitId, static_cast<unsigned>(token),
        static_cast<double>(params.anchor.x),
        static_cast<double>(params.anchor.y),
        static_cast<double>(params.anchor.z));

    return token;
}

bool isStageExitReadyToCommit() {
    return g_deferredExit.hasPending && g_deferredExit.committed;
}

void commitStageExitNow() {
    if (!g_deferredExit.hasPending || !g_deferredExit.committed) {
        return;
    }

    const CapturedExitParams& p = g_deferredExit.captured;

#if TARGET_PC
    if (p.groundPath) {
        cBgS_PolyInfo groundPoly;
        groundPoly.SetPolyInfo(p.groundPoly);
        dStage_changeSceneExitId(groundPoly, p.speed, p.mode,
                                 p.roomNo, p.angle);
    } else {
        dStage_changeScene(p.exitId, p.speed, p.mode, p.roomNo, p.angle, p.param5);
    }
    s_stageExitCommittedThisFrame = true;
    debug::logInfo(
        "event: commitStageExitNow exitId=%d roomNo=%d groundPath=%d",
        p.exitId, static_cast<int>(p.roomNo), p.groundPath ? 1 : 0);
#else
    (void)p;
#endif

    g_deferredExit = {};
}

const CapturedExitParams& getCommittedExitParams() {
    static CapturedExitParams s_empty{};
    if (!g_deferredExit.hasPending || !g_deferredExit.committed) {
        return s_empty;
    }
    return g_deferredExit.captured;
}

bool isStageExitPending() {
    return g_deferredExit.hasPending && !g_deferredExit.committed;
}

bool wasStageExitCommittedThisFrame() {
    return s_stageExitCommittedThisFrame;
}

// ===========================================================================
// Active PartyStory token accessors
// ===========================================================================

EventToken getActivePartyStoryToken() {
    return s_activePartyStoryToken;
}

void setActivePartyStoryToken(EventToken token) {
    s_activePartyStoryToken = token;
}

// ===========================================================================
// Conversation API
// ===========================================================================

EventToken trackConversation(const CapturedConversationParams& params) {
    // Dedup: if there's already an active conversation, reject new ones.
    if (s_activeConversationToken != INVALID_EVENT_TOKEN) {
        const EventSlot* existing = slotFor(s_activeConversationToken);
        if (existing != nullptr && existing->state == EventState::Running) {
            debug::logInfo(
                "event: trackConversation dedup token=%u initiator=P%u "
                "profName=0x%04x eventId=%u",
                static_cast<unsigned>(s_activeConversationToken),
                static_cast<unsigned>(existing->request.initiator),
                static_cast<unsigned>(params.profName),
                static_cast<unsigned>(params.eventId));
            return s_activeConversationToken;
        }
        // Stale token — clear it.
        s_activeConversationToken = INVALID_EVENT_TOKEN;
        s_activeConversationParams = {};
    }

    if (params.initiator >= MAX_LOCAL_PLAYERS) {
        debug::logWarn("event: trackConversation invalid initiator=%u",
                       static_cast<unsigned>(params.initiator));
        return INVALID_EVENT_TOKEN;
    }

    const EventToken token = allocSlot();
    if (token == INVALID_EVENT_TOKEN) {
        debug::logError("event: trackConversation no free slots");
        return INVALID_EVENT_TOKEN;
    }

    EventSlot& slot = g_events[token];
    slot.active = true;
    slot.request.kind = EventKind::Conversation;
    slot.request.scope = EventScope::InitiatorOwned;
    slot.request.initiator = params.initiator;
    slot.request.presentationOwner = params.initiator;  // initiator owns presentation
    slot.request.anchor = params.anchor;
    slot.request.radius = 0.0f;  // no proximity check for conversations
    slot.request.eventId = params.eventId;
    slot.request.extraFlags = static_cast<u32>(params.mapToolId);
    slot.state = EventState::Running;  // skip WaitingForParty — immediate
    slot.participantsMask = 0;
    for (uint8_t i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (isJoined(i)) {
            slot.participantsMask |= (1u << i);
        }
    }
    slot.readinessMask = slot.participantsMask;  // all ready (no barrier)
    slot.tickCounter = 0;

    s_activeConversationToken = token;
    s_activeConversationParams = params;

    debug::logInfo(
        "event: trackConversation token=%u initiator=P%u "
        "profName=0x%04x eventId=%u stage=%.4s room=%d "
        "anchor=(%.0f,%.0f,%.0f)",
        static_cast<unsigned>(token),
        static_cast<unsigned>(params.initiator),
        static_cast<unsigned>(params.profName),
        static_cast<unsigned>(params.eventId),
        params.stageName,
        static_cast<int>(params.roomNo),
        static_cast<double>(params.anchor.x),
        static_cast<double>(params.anchor.y),
        static_cast<double>(params.anchor.z));

    // Activate the render presentation override so this conversation renders
    // full-screen from the initiator's camera (Step 6).
    render::pushConversationPresentation(params.initiator);

    return token;
}

EventToken getActiveConversationToken() {
    return s_activeConversationToken;
}

void setActiveConversationToken(EventToken token) {
    s_activeConversationToken = token;
}

bool markConversationFinished(EventToken token) {
    if (s_activeConversationToken != token) {
        debug::logWarn(
            "event: markConversationFinished token=%u != active=%u "
            "— ignoring stale finish",
            static_cast<unsigned>(token),
            static_cast<unsigned>(s_activeConversationToken));
        if (s_activeConversationToken != INVALID_EVENT_TOKEN) {
            s_activeConversationToken = INVALID_EVENT_TOKEN;
            s_activeConversationParams = {};
        }
        return false;
    }

    EventSlot* slot = slotFor(token);
    if (slot == nullptr) {
        s_activeConversationToken = INVALID_EVENT_TOKEN;
        s_activeConversationParams = {};
        return false;
    }

    const EventState oldState = slot->state;
    if (oldState != EventState::Running) {
        debug::logWarn(
            "event: markConversationFinished token=%u state=%s != Running"
            " — ignoring invalid transition",
            static_cast<unsigned>(token),
            stateName(oldState));
        return false;
    }

    // Deactivate the render presentation override (Step 6).
    render::popConversationPresentation();

    // Distribute rewards before clearing the token.
    applyConversationRewards(s_activeConversationParams);

    slot->active = false;
    slot->state = EventState::Idle;
    s_activeConversationToken = INVALID_EVENT_TOKEN;
    s_activeConversationParams = {};

    debug::logInfo(
        "event: conversation finish token=%u -> Idle (reclaimed)",
        static_cast<unsigned>(token));
    return true;
}

// ---------------------------------------------------------------------------
// Per-player reward distribution helpers — these map item constants from the
// vanilla event system to per-player co-op resource APIs.  Only resource/
// consumable types are supported.  Equipment, key items, dungeon items, and
// quest items fail closed (logged and skipped) because duplicating them
// for every player would replicate unsafe vanilla side effects.
// ---------------------------------------------------------------------------

namespace {

// Map a rupee item constant to its rupee-amount value.
// Returns 0 for non-rupee items.
s16 rupeeAmount(u8 itemNo) {
    switch (itemNo) {
    case dItemNo_GREEN_RUPEE_e:  return 1;
    case dItemNo_BLUE_RUPEE_e:   return 5;
    case dItemNo_YELLOW_RUPEE_e: return 10;
    case dItemNo_RED_RUPEE_e:    return 20;
    case dItemNo_PURPLE_RUPEE_e: return 50;
    case dItemNo_ORANGE_RUPEE_e: return 100;
    case dItemNo_SILVER_RUPEE_e: return 200;
    default:                     return 0;
    }
}

// Return quarter-hearts for heart-type pickups.
// Returns 0 for non-heart items.
s16 heartQuarterHearts(u8 itemNo) {
    switch (itemNo) {
    case dItemNo_HEART_e:         return 4;   // 1 heart
    case dItemNo_TRIPLE_HEART_e:  return 12;  // 3 hearts
    case dItemNo_RECOVERY_FAILY_e: return 32; // full-heal equivalent (8 hearts)
    default:                      return 0;
    }
}

// Return magic units for magic pickups.
// Returns 0 for non-magic items.
u8 magicUnits(u8 itemNo) {
    switch (itemNo) {
    case dItemNo_S_MAGIC_e: return 4;
    case dItemNo_L_MAGIC_e: return 8;
    default:                return 0;
    }
}

// Return arrow count for arrow pickups.
// Returns 0 for non-arrow items.
u8 arrowCount(u8 itemNo) {
    switch (itemNo) {
    case dItemNo_ARROW_1_e:  return 1;
    case dItemNo_ARROW_10_e: return 10;
    case dItemNo_ARROW_20_e: return 20;
    case dItemNo_ARROW_30_e: return 30;
    default:                 return 0;
    }
}

// Return (bombBagIdx, count) for bomb-type items.
// Returns (0xFF, 0) for non-bomb items.
struct BombAmount { u8 bagIdx; u8 count; };
BombAmount bombAmount(u8 itemNo) {
    switch (itemNo) {
    case dItemNo_BOMB_5_e:          return {0, 5};
    case dItemNo_BOMB_10_e:         return {0, 10};
    case dItemNo_BOMB_20_e:         return {0, 20};
    case dItemNo_BOMB_30_e:         return {0, 30};
    case dItemNo_WATER_BOMB_5_e:    return {1, 5};
    case dItemNo_WATER_BOMB_10_e:   return {1, 10};
    case dItemNo_WATER_BOMB_20_e:   return {1, 15};
    case dItemNo_WATER_BOMB_30_e:   return {1, 3};
    case dItemNo_BOMB_INSECT_5_e:   return {2, 5};
    case dItemNo_BOMB_INSECT_10_e:  return {2, 10};
    case dItemNo_BOMB_INSECT_20_e:  return {2, 3};
    // BOMB_INSECT_30_e is a no-op in vanilla — skip.
    default:                         return {0xFF, 0};
    }
}

// Distribute a rupee reward to one player.
void giveRupees(PlayerId id, s16 amount) {
    if (amount <= 0) return;
    inventory::tryAddRupees(id, amount);
    debug::logInfo(
        "event: reward player=%u +%d rupees",
        static_cast<unsigned>(id), static_cast<int>(amount));
}

// Distribute a heart reward to one player.
void giveHearts(PlayerId id, s16 qHearts) {
    if (qHearts <= 0) return;
    auto& res = inventory::resources(id);
    s32 newLife = static_cast<s32>(res.life) + static_cast<s32>(qHearts);
    if (newLife > res.maxLife) newLife = res.maxLife;
    if (newLife < 0) newLife = 0;
    res.life = static_cast<s16>(newLife);
    debug::logInfo(
        "event: reward player=%u +%d quarter-hearts (now %d/%d)",
        static_cast<unsigned>(id), static_cast<int>(qHearts),
        static_cast<int>(res.life), static_cast<int>(res.maxLife));
}

// Distribute magic to one player.
void giveMagic(PlayerId id, u8 amount) {
    if (amount == 0) return;
    auto& res = inventory::resources(id);
    s32 newMagic = static_cast<s32>(res.magic) + static_cast<s32>(amount);
    if (newMagic > res.maxMagic) newMagic = res.maxMagic;
    if (newMagic < 0) newMagic = 0;
    res.magic = static_cast<u16>(newMagic);
    debug::logInfo(
        "event: reward player=%u +%d magic (now %d/%d)",
        static_cast<unsigned>(id), static_cast<int>(amount),
        static_cast<int>(res.magic), static_cast<int>(res.maxMagic));
}

// Distribute arrows to one player.
void giveArrows(PlayerId id, u8 count) {
    if (count == 0) return;
    inventory::tryAddArrows(id, count);
    debug::logInfo(
        "event: reward player=%u +%d arrows",
        static_cast<unsigned>(id), static_cast<unsigned>(count));
}

// Distribute bombs to one player.
void giveBombs(PlayerId id, u8 bagIdx, u8 count) {
    if (bagIdx == 0xFF || count == 0) return;
    inventory::tryAddBombs(id, bagIdx, count);
    debug::logInfo(
        "event: reward player=%u +%d bombs (bag %u)",
        static_cast<unsigned>(id), static_cast<unsigned>(count),
        static_cast<unsigned>(bagIdx));
}

// Distribute pachinko balls to one player.
void givePachinko(PlayerId id) {
    // Vanilla item_func_PACHINKO_SHOT grants 50.
    inventory::tryAddPachinko(id, 50);
    debug::logInfo(
        "event: reward player=%u +50 pachinko",
        static_cast<unsigned>(id));
}

// Refill oil for one player (full tank).
void giveFullOil(PlayerId id) {
    auto& res = inventory::resources(id);
    res.oil = res.maxOil;
    debug::logInfo(
        "event: reward player=%u oil refilled to %d/%d",
        static_cast<unsigned>(id),
        static_cast<int>(res.oil), static_cast<int>(res.maxOil));
}

// Give a bottle unlock to one player.
// For empty bottles: grants a new bottle slot (empty).
// For filled bottles: grants a new bottle slot with contents.
void giveBottle(PlayerId id, u8 itemNo) {
    if (isBottleItem(itemNo)) {
        u8 contents = dItemNo_EMPTY_BOTTLE_e;
        if (itemNo != dItemNo_EMPTY_BOTTLE_e) {
            contents = itemNo;
        }
        if (bottles::grantBottleUnlock(id, contents)) {
            debug::logInfo(
                "event: reward player=%u bottle unlock (item=0x%02x)",
                static_cast<unsigned>(id),
                static_cast<unsigned>(itemNo));
        } else {
            debug::logInfo(
                "event: reward player=%u bottle unlock skipped "
                "(all slots already unlocked, item=0x%02x)",
                static_cast<unsigned>(id),
                static_cast<unsigned>(itemNo));
        }
    }
}

}  // namespace

void applyConversationRewards(const CapturedConversationParams& params) {
#if TARGET_PC
    // Current policy: every joined player receives the conversation reward.
    // Global flags were already committed once by the vanilla event — we do
    // NOT duplicate those.
    //
    // The item granted by the event is read from dComIfGp_event_getGtItm().
    // Each joined player gets a copy of this item (or equivalent resource).
    //
    // For story-critical items, only the initiator should receive
    // the physical item; other players get a notification/flag sync.
    // For consumables (rupees, bombs, arrows) every player gets a copy.

    const u8 grantedItem = dComIfGp_event_getGtItm();
    if (grantedItem == 0 || grantedItem == 0xFF) {
        debug::logInfo(
            "event: applyConversationRewards no item granted "
            "(gtItm=%u)",
            static_cast<unsigned>(grantedItem));
        return;
    }

    // Determine what kind of reward this is and its amount.
    const s16 rupeeAmt = rupeeAmount(grantedItem);
    const s16 heartQH = heartQuarterHearts(grantedItem);
    const u8 magicAmt = magicUnits(grantedItem);
    const u8 arrowCnt = arrowCount(grantedItem);
    const BombAmount bombAmt = bombAmount(grantedItem);
    const bool isPachinko = (grantedItem == dItemNo_PACHINKO_SHOT_e);
    const bool isOil = (grantedItem == dItemNo_OIL_e ||
                        grantedItem == dItemNo_OIL2_e);
    const bool isBottle = isBottleItem(grantedItem);

    // Guard: if this item type is not supported for per-player distribution,
    // log it clearly and return without duplicating unsafe side effects.
    const bool supported = (rupeeAmt > 0) || (heartQH > 0) || (magicAmt > 0) ||
                           (arrowCnt > 0) || (bombAmt.bagIdx != 0xFF) ||
                           isPachinko || isOil || isBottle;

    if (!supported) {
        debug::logInfo(
            "event: applyConversationRewards unsupported item 0x%02x "
            "(profName=0x%04x eventId=%u initiator=P%u) — no distribution. "
            "FUTURE: add mapping for this item type in the switch below, "
            "or verify it should NOT be distributed party-wide",
            static_cast<unsigned>(grantedItem),
            static_cast<unsigned>(params.profName),
            static_cast<unsigned>(params.eventId),
            static_cast<unsigned>(params.initiator));
        return;
    }

    debug::logInfo(
        "event: applyConversationRewards initiator=P%u "
        "profName=0x%04x eventId=%u item=0x%02x — distributing to all "
        "joined players",
        static_cast<unsigned>(params.initiator),
        static_cast<unsigned>(params.profName),
        static_cast<unsigned>(params.eventId),
        static_cast<unsigned>(grantedItem));

    for (uint8_t i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (!isJoined(i)) continue;

        if (rupeeAmt > 0) {
            giveRupees(i, rupeeAmt);
        }
        if (heartQH > 0) {
            giveHearts(i, heartQH);
        }
        if (magicAmt > 0) {
            giveMagic(i, magicAmt);
        }
        if (arrowCnt > 0) {
            giveArrows(i, arrowCnt);
        }
        if (bombAmt.bagIdx != 0xFF && bombAmt.count > 0) {
            giveBombs(i, bombAmt.bagIdx, bombAmt.count);
        }
        if (isPachinko) {
            givePachinko(i);
        }
        if (isOil) {
            giveFullOil(i);
        }
        if (isBottle) {
            giveBottle(i, grantedItem);
        }
    }
#else
    (void)params;
#endif
}

}  // namespace dusk::coop::event

// ===========================================================================
// Bridge function implementations (extern "C" — must be at global scope
// per coop_event_bridge.h declarations)
// ===========================================================================

#if TARGET_PC

extern "C" {

// Conservative route-blocking NPC/event classifier.
//
// IMPORTANT: This function discriminates by EVENT + STAGE + ROOM, NOT by
// broad profile type alone.  Many NPC profile types (e.g. KN, BOU, KOLIN)
// are used for dozens of ordinary conversations across the game.  Classifying
// every NPC of that type as PartyStory would gate every mundane interaction
// behind a party barrier.
//
// Until a concrete event+stage+room combination is confirmed (e.g. through
// debug logs), the function returns 0 and the candidate is logged for
// discovery via dusk_coop_logPartyStoryCandidate.
//
// Currently recognized route-blocking event contexts (populated from
// confirmed discovery logs + stage data):
//   (none yet — first discovery must provide eventId + stage + room)
//
// When a new context is confirmed, add it here as e.g.:
//   if (profName == fpcNm_Tag_Mwait_e &&
//       eventId == SOME_EVENT_ID &&
//       stageName && !strcmp(stageName, "R_SP100") &&
//       roomNo == 0) return 1;
u8 dusk_coop_isPartyStoryEvent(s16 profName, u16 eventId,
                                const char* stageName, s8 roomNo) {
    (void)eventId;
    (void)stageName;
    (void)roomNo;
    // NOTE: fpcNm_Tag_Mwait_e and fpcNm_Tag_Hstop_e are dedicated trigger
    // tags (not generic NPCs), but even they may appear in non-blocking
    // contexts.  Add stage/room/eventId guards once discovered.
    //
    // For now, return 0 for EVERYTHING — no concrete event+stage+room
    // combination has been confirmed yet.  The debug logs from the first
    // playthrough will provide the data needed here.
    return 0;
}

void dusk_coop_logPartyStoryCandidate(s16 profName, u16 eventId,
                                       const char* stageName, s8 roomNo) {
    const bool match = dusk_coop_isPartyStoryEvent(profName, eventId, stageName, roomNo);
    dusk::coop::debug::logInfo(
        "PARTYSTORY-CANDIDATE: profName=0x%04x eventId=%u stage=%s room=%d "
        "classifier=%s",
        static_cast<unsigned>(profName),
        static_cast<unsigned>(eventId),
        stageName ? stageName : "?",
        static_cast<int>(roomNo),
        match ? "MATCH" : "UNMATCHED");
}

}  // extern "C"

#endif  // TARGET_PC
