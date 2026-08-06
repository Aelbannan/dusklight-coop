#include "dusk/coop/coop.h"

#include "dusk/frame_interpolation.h"

#include "dusk/coop/coop_combat.h"
#include "dusk/coop/coop_enemy.h"
#include "dusk/coop/coop_time.h"
#include "dusk/net/discovery.h"
#include "dusk/ui/ui.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "dusk/config.hpp"
#include "dusk/net/config.h"
#include "dusk/net/session.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_layer.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "SSystem/SComponent/c_math.h"
#include "SSystem/SComponent/c_phase.h"
#include "SSystem/SComponent/c_sxyz.h"
#include "SSystem/SComponent/c_xyz.h"

#include <JSystem/JKernel/JKRExpHeap.h>

#include <aurora/lib/logging.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dusk::coop {

namespace {

aurora::Module CoopLog("dusk::coop");

using net::kInvalidPlayerId;
using net::kMaxLocalPlayers;
using net::PlayerId;

// ---------------------------------------------------------------------------
// Session glue — the one Session the game runs, driven from onGameFrame.
// ---------------------------------------------------------------------------

net::Session g_session;
bool g_sessionStarted = false;
bool g_wasLive = false;
bool g_startFailed = false;
u32 g_startFailFrame = 0;
// Capstone MINOR F (review-full-deepseek-v4-flash-0731.md MINOR F): one-shot
// notification latch — a join rejection / start failure toasts once per
// occurrence instead of spamming the 3 s retry loop.
bool g_rejectedNotified = false;

// ---------------------------------------------------------------------------
// Real Link tracking (the local player's own daAlink_c per stage).
// ---------------------------------------------------------------------------

daAlink_c* g_realLink = nullptr;
fpc_ProcID g_realLinkPid = fpcM_ERROR_PROCESS_ID_e;
bool g_realLinkReady = false;

// ---------------------------------------------------------------------------
// Puppet registry — per-stage, indexed by session PlayerId. Entries survive
// the actor's death (stage change / leave) until ~daAlink_c runs and clears
// them, so the destructor's slot-0 guards keep firing for the whole lifetime
// of a puppet process.
// ---------------------------------------------------------------------------

enum class SpawnState : u8 {
    None,       // no puppet for this player
    Requested,  // player present, spawn queued (serialized, one in flight)
    Creating,   // fopAcM_create issued; create() still multi-phase
    Active,     // create completed; puppet driven by received state
    Despawning, // fopAcM_delete issued; waiting for the destructor to clear
};

struct PuppetEntry {
    SpawnState state = SpawnState::None;
    fpc_ProcID pid = fpcM_ERROR_PROCESS_ID_e;
    daAlink_c* actor = nullptr;
    bool hidden = false;
    u32 retryFrame = 0;
    u32 createStartFrame = 0;  // g_frameCount when the create was issued (M3 deadline)
};

std::array<PuppetEntry, kMaxLocalPlayers> g_puppets{};
bool g_createInFlight = false;

// MAJOR M3: daAlink_c::create() phase 2 can spin on cPhs_INIT_e forever
// (ground check over a cliff at the +120-unit spawn offset, a residual
// ride/portal wait) without the process dying, so onLinkCreated never fires
// and g_createInFlight would block every later spawn for ANY player. The
// pump aborts a create that exceeds kCreateDeadlineFrames; after
// kCreateDeadlineStrikes consecutive aborts it drops the spawn until the
// player leaves or a create succeeds.
constexpr u32 kCreateDeadlineFrames = 600;  // 10 s @ 60 Hz
constexpr u8 kCreateDeadlineStrikes = 3;

struct CreateLimiter {
    u8 deadlineHits = 0;
    bool dropped = false;  // 0xFFFFFFFF suppression window (persists across entry resets)
    // Capstone MINOR G: the real-Link anchor (room + position) where the
    // create stuck, so the strike budget can be reset when the host moves
    // away materially (a cliff edge is position-specific).
    bool hasAnchor = false;
    s8 anchorRoom = -1;
    f32 anchor[3] = {0.0f, 0.0f, 0.0f};
};
std::array<CreateLimiter, kMaxLocalPlayers> g_createLimiter{};

// ---------------------------------------------------------------------------
// Receive slots — latest PlayerState / PlayerEvent per remote player.
// ---------------------------------------------------------------------------

struct ReceiveSlot {
    bool hasState = false;
    bool hasEvent = false;
    net::PlayerStateMsg state{};
    net::PlayerEventMsg event{};
    // This machine's room/stage when the remote last sent us PlayerState. Used
    // by the sender gate (MAJOR M2): a remote's stale room can gate us silent
    // forever; if OUR room/stage changed since their last send, the gate opens
    // so they learn where we are. The stage half is the cross-stage analog: a
    // remote on another stage (spring / house interior) must not keep us gated.
    s8 myRoomAtLastRecv = -1;
    char myStageAtLastRecv[net::kMaxStageNameLength] = {};
};

std::array<ReceiveSlot, kMaxLocalPlayers> g_receive{};

// ---------------------------------------------------------------------------
// Form-swap state — a puppet's create only loads the host save's arc; when
// the received form differs, the other form's arc is loaded on demand with
// the vanilla metamorphose sequence (resDelete + cPhs_Reset + freeAll +
// setArcName + resLoad), then changeWolf()/changeLink(0) swap the skeleton.
// The object-res system refcounts shared arcs, so freeing a puppet's arc
// never invalidates the real Link's model data.
// ---------------------------------------------------------------------------

struct FormSwapState {
    bool active = false;
    bool wantWolf = false;
};

std::array<FormSwapState, kMaxLocalPlayers> g_formSwap{};

// ---------------------------------------------------------------------------
// Sender change tracking (reliable PlayerEvent on change only).
// ---------------------------------------------------------------------------

u32 g_lastSentForm = 0xFFFFFFFF;
u16 g_lastSentEquip = 0xFFFF;
u8 g_lastSentSelectItem = 0xFF;
u16 g_lastSentLeftJnt = 0xFFFF;
u16 g_lastSentRightJnt = 0xFFFF;
u32 g_lastSentAttention = 0xFFFFFFFF;
s8 g_lastSentRoom = 0x7F;
// Stage of the last PlayerState we actually sent (NUL when none yet). Used to
// detect a stage change even when the room number coincides (F_SP103 room 1 ->
// F_SP104 room 1), so the reliable SceneChange + send window fire on stage
// changes too — otherwise a mover's new stage is carried by a single
// unreliable state and a drop leaves a frozen puppet in the old room.
char g_lastSentStage[net::kMaxStageNameLength] = {};
// Frames left in the post-stage/room-change send window: after moving, keep
// sending regardless of the gate so the new stage/room reaches the remote
// even if the first few unreliable states drop (the gate's "we moved" check
// closes after one inbound reply, which can be earlier than any of our
// post-move states land).
u32 g_postChangeSendWindow = 0;
u32 g_frameCount = 0;

// ---------------------------------------------------------------------------
// M4 state — host-leave UX (D8), LAN discovery
// ---------------------------------------------------------------------------

// LAN discovery lifecycle (host announces, clients listen).
dusk::net::discovery::Announcer g_discoveryAnnouncer;
dusk::net::discovery::Listener g_discoveryListener;
bool g_announcerActive = false;
bool g_listenerActive = false;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Forward declaration: defined in the M4 helpers section below (used by
// EnsureSession's capstone MINOR F start-failure toast).
void NotifyCoop(const char* title, const char* content);

bool SessionLive() {
    if (!g_sessionStarted) {
        return false;
    }
    if (g_session.role() == net::SessionRole::Host) {
        return g_session.state() == net::SessionState::Listening ||
               g_session.state() == net::SessionState::Joined;
    }
    return g_session.state() == net::SessionState::Joined;
}

PlayerId SelfIdChecked() {
    if (g_session.role() == net::SessionRole::Host) {
        return 0;
    }
    return g_session.selfId();
}

const PuppetEntry* EntryForPid(fpc_ProcID pid) {
    if (pid == fpcM_ERROR_PROCESS_ID_e) {
        return nullptr;
    }
    for (const auto& e : g_puppets) {
        if (e.pid == pid && (e.state == SpawnState::Creating || e.state == SpawnState::Active ||
                             e.state == SpawnState::Despawning))
        {
            return &e;
        }
    }
    return nullptr;
}

s8 LocalRoomNo() {
    if (g_realLink != nullptr) {
        return fopAcM_GetRoomNo(g_realLink);
    }
    return -1;
}

/// The local player's CURRENT stage (dComIfGp_getStartStageName tracks the
/// play's start-stage object, which is re-pointed on every stage change).
const char* LocalStageName() {
    return dComIfGp_getStartStageName();
}

void ClearAllPuppets() {
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        PuppetEntry& e = g_puppets[i];
        if (e.state == SpawnState::Creating || e.state == SpawnState::Active) {
            if (e.actor != nullptr) {
                fopAcM_delete(e.actor);
            } else if (e.pid != fpcM_ERROR_PROCESS_ID_e) {
                fopAcM_delete(e.pid);
            }
            // The destructor clears the entry; keep it flagged until then so
            // the slot-0 guards in ~daAlink_c keep engaging.
            e.state = SpawnState::Despawning;
        }
    }
    g_createInFlight = false;
}

// ---------------------------------------------------------------------------
// Receive handler (session game-message hook) — called inside Session::Update
// on the game thread, before the actor phase, so a puppet's own update later
// in the frame consumes the freshest state.
// ---------------------------------------------------------------------------

void OnGameMessage(net::MsgType type, const net::PayloadUnion& payload) {
    if (type == net::MsgType::PlayerState) {
        const auto& st = payload.playerState;
        if (st.playerId >= kMaxLocalPlayers || st.playerId == SelfIdChecked()) {
            return;
        }
        ReceiveSlot& slot = g_receive[st.playerId];
        slot.state = st;
        slot.hasState = true;
        slot.myRoomAtLastRecv = LocalRoomNo();
        std::snprintf(slot.myStageAtLastRecv, sizeof(slot.myStageAtLastRecv), "%s",
                      LocalStageName());
    } else if (type == net::MsgType::PlayerEvent) {
        const auto& ev = payload.playerEvent;
        if (ev.playerId >= kMaxLocalPlayers || ev.playerId == SelfIdChecked()) {
            return;
        }
        ReceiveSlot& slot = g_receive[ev.playerId];
        slot.event = ev;
        slot.hasEvent = true;
        // MAJOR M1 (room visibility): PlayerState is unreliable and the
        // sender's room-change send window is ~1 frame (the sender gate
        // closes as soon as our reply lands), so the PlayerState carrying the
        // remote's NEW room can drop while the reliable SceneChange event
        // always arrives. Adopt the event's room into the slot now so the
        // puppet's hidden gate (st.roomNo != local room) is deterministic
        // instead of depending on an unreliable packet — otherwise a remote
        // who left the room keeps rendering (and animating) on our screen.
        if (static_cast<net::PlayerEventId>(ev.eventId) == net::PlayerEventId::SceneChange) {
            // Capstone MINOR A (review-full-deepseek-v4-flash-0731.md MINOR A):
            // adopt the event's room only for SAME-STAGE moves (scene == 1),
            // exactly like the host's room-table sniff (session.cpp). A
            // cross-stage mover sends scene=0; adopting (oldStage, newRoom)
            // could coincide with our local room and flash the mover's puppet
            // visible — the hidden gate `sameStage && roomNo == local` would
            // hold — until the first new-stage PlayerState lands. Cross-stage
            // moves are established by PlayerState only, which writes the
            // room byte in the same message.
            if (ev.scene == 1) {
                slot.state.roomNo = static_cast<s8>(ev.data & 0xFF);
            }
        }
    } else if (type == net::MsgType::EnemySnapshot || type == net::MsgType::EnemyEvent) {
        // M2: enemy authority traffic (freeze/apply + drops/room-clear).
        dusk::coop::enemy::onGameMessage(type, payload);
    } else if (type == net::MsgType::CombatIntent) {
        // M2: combat intents are validated by the sim owner (host role).
        dusk::coop::combat::onGameMessage(type, payload);
    } else if (type == net::MsgType::CombatResult) {
        // M2: result ack — the authoritative HP always rides the next
        // EnemySnapshot; nothing to apply client-side in v1. Capstone MINOR 2
        // (review-full-glm-5.2.md MINOR 2): this receive side is INTENTIONALLY
        // unconsumed in v1 — no consumer exists anywhere (the ack is
        // star-relayed dead traffic today). M5 decides wire-vs-drop; do NOT
        // change behavior here.
    } else if (type == net::MsgType::TimeSync || type == net::MsgType::TimeEvent ||
               type == net::MsgType::WeatherChange)
    {
        // M3: the client clock/weather replica (absolute adopt, events,
        // weather target + re-pin). The host never receives these from a
        // peer (they are host-generated, host->all); a forged inbound copy is
        // ignored here.
        dusk::coop::timeweather::onGameMessage(type, payload);
    }
}

// ---------------------------------------------------------------------------
// Sender
// ---------------------------------------------------------------------------

void SendPlayerEvent(net::PlayerEventId eventId, u32 data, u32 data2, u8 scene = 0) {
    net::PayloadUnion payload = {};
    payload.playerEvent.playerId = SelfIdChecked();
    payload.playerEvent.eventId = static_cast<u8>(eventId);
    payload.playerEvent.scene = scene;
    payload.playerEvent.data = data;
    payload.playerEvent.data2 = data2;
    if (!g_session.SendGameMessage(net::MsgType::PlayerEvent, payload)) {
        CoopLog.debug("coop: dropped PlayerEvent {} (session not sendable)", static_cast<u8>(eventId));
    }
}

/// Maps the local Link's lock target to a session entity id (a remote player
/// id) or kInvalidPlayerId (0xFFFF) when it is local-only / none.
u32 AttentionTargetId(const daAlink_c* link) {
    fopAc_ac_c* target = link->mTargetedActor;
    if (target == nullptr) {
        return kInvalidPlayerId;
    }
    const PlayerId pid = puppetPlayerId(fopAcM_GetID(target));
    return pid != kInvalidPlayerId ? static_cast<u32>(pid) : kInvalidPlayerId;
}

void ResetSenderTrackers() {
    g_lastSentForm = 0xFFFFFFFF;
    g_lastSentEquip = 0xFFFF;
    g_lastSentSelectItem = 0xFF;
    g_lastSentLeftJnt = 0xFFFF;
    g_lastSentRightJnt = 0xFFFF;
    g_lastSentAttention = 0xFFFFFFFF;
    g_lastSentRoom = 0x7F;
    g_lastSentStage[0] = '\0';
    g_postChangeSendWindow = 0;
}

void SendEventsOnChange(const daAlink_c* link) {
    const u32 form = link->checkWolf() ? 1u : 0u;
    if (form != g_lastSentForm) {
        g_lastSentForm = form;
        SendPlayerEvent(net::PlayerEventId::FormChange, form, 0);
    }

    const u16 equip = link->mEquipItem;
    const u8 selectItem = link->mSelectItemId;
    const u16 leftJnt = link->mLeftItemJntNo;
    const u16 rightJnt = link->mRightItemJntNo;
    if (equip != g_lastSentEquip || selectItem != g_lastSentSelectItem ||
        leftJnt != g_lastSentLeftJnt || rightJnt != g_lastSentRightJnt)
    {
        g_lastSentEquip = equip;
        g_lastSentSelectItem = selectItem;
        g_lastSentLeftJnt = leftJnt;
        g_lastSentRightJnt = rightJnt;
        SendPlayerEvent(net::PlayerEventId::Equip, static_cast<u32>(equip) |
                                                       (static_cast<u32>(selectItem) << 16),
            static_cast<u32>(leftJnt) | (static_cast<u32>(rightJnt) << 16));
    }

    const u32 attention = AttentionTargetId(link);
    if (attention != g_lastSentAttention) {
        g_lastSentAttention = attention;
        SendPlayerEvent(net::PlayerEventId::AttentionChange, attention, 0);
    }
    // The room-change event is NOT sent here: sendPlayerState() sends it
    // before the sender gate so a room change always propagates (MAJOR M2).
}

/// Sender gate (Anchor model): only send when at least one remote player is
/// in (or unknown to be outside) our scene — a client only renders peers in
/// its own stage+room, so same-scene peers are the only ones that can see us.
///
/// Capstone MINOR L (review-full-deepseek-v4-flash-0731.md MINOR L): this is
/// THE sender gate — ONE implementation shared by the player sender
/// (sendPlayerState) and the enemy snapshot sender (coop_enemy.cpp
/// RemoteInRoom -> dusk::coop::remoteInRoom). The two previously duplicated
/// the same rule with slightly different structure; a single implementation
/// cannot drift as M5 adds more senders (waves, horses).
///
/// MAJOR M2 (mutual room-change deadlock): two players entering the same new
/// room together hold each other's stale room, so `state.roomNo == myRoom`
/// is false on both sides and both gates would stay shut forever (both
/// puppets hidden). The gate therefore also opens when OUR stage/room changed
/// since the remote last sent us state — they cannot know where we are, so
/// we send; one PlayerState with the new room re-opens their gate. The same
/// holds for a cross-stage move (spring / house interior), where the room
/// numbers are not unique across stages.
bool RemoteInOurRoom(const char* stage, s8 roomNo) {
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        if (i == SelfIdChecked()) {
            continue;
        }
        if (!g_session.roster()[i].present) {
            continue;
        }
        const ReceiveSlot& slot = g_receive[i];
        // Unknown room (never received state) opens the gate — the Anchor
        // rule: don't hold state hostage to a missing packet.
        if (!slot.hasState) {
            return true;
        }
        const bool sameStage = std::strcmp(slot.state.stage, stage) == 0;
        if (sameStage && slot.state.roomNo == roomNo) {
            return true;
        }
        if (slot.myRoomAtLastRecv != roomNo ||
            std::strcmp(slot.myStageAtLastRecv, stage) != 0)
        {
            return true;  // our stage/room changed since their last send
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Puppet apply — exact order per 01-puppet-link.md §6.4 / implementation-plan
// Rev 3 §5 M1: transform -> form -> mProcID -> face -> setMatrix ->
// allAnimePlay -> face frame -> model calc -> matrix pose copy + baseTR ->
// item matrices -> body parts -> attention -> collision -> Tg registration ->
// Acch -> room info.
// ---------------------------------------------------------------------------

void ApplyPendingEvent(daAlink_c* link, PlayerId pid) {
    ReceiveSlot& slot = g_receive[pid];
    if (!slot.hasEvent) {
        return;
    }
    const net::PlayerEventMsg& ev = slot.event;
    if (ev.playerId != pid) {
        slot.hasEvent = false;
        return;
    }
    switch (static_cast<net::PlayerEventId>(ev.eventId)) {
    case net::PlayerEventId::Equip: {
        const u16 equip = static_cast<u16>(ev.data & 0xFFFF);
        const u8 selectItem = static_cast<u8>((ev.data >> 16) & 0xFF);
        const u16 leftJnt = static_cast<u16>(ev.data2 & 0xFFFF);
        const u16 rightJnt = static_cast<u16>((ev.data2 >> 16) & 0xFFFF);
        link->mEquipItem = equip;
        link->mSelectItemId = selectItem;
        if (leftJnt != 0xFFFF) {
            link->mLeftItemJntNo = leftJnt;
        }
        if (rightJnt != 0xFFFF) {
            link->mRightItemJntNo = rightJnt;
        }
        CoopLog.debug("coop: player {} equip {} sel {} joints {}/{}", pid, equip, selectItem,
            leftJnt, rightJnt);
        break;
    }
    default:
        // FormChange / AttentionChange are either carried per frame in
        // PlayerState (form) or not used for rendering (M1). SceneChange is
        // consumed at receive time (OnGameMessage) — the reliable event is
        // the room-change authority so a dropped PlayerState can't leave a
        // room-leaver's puppet visible; PlayerState.roomNo then keeps it
        // current while states flow.
        break;
    }
    slot.hasEvent = false;
}

/// Drives the puppet's form swap (human <-> wolf) across frames. Returns
/// true once the puppet's skeleton matches `wantWolf` (either it already
/// does, or the target arc finished loading and changeWolf/changeLink ran).
/// While the arc is loading the caller must hold the pose and hide the
/// puppet (its current model data was freed).
static bool DriveFormSwap(daAlink_c* link, PlayerId pid, bool wantWolf) {
    FormSwapState& fs = g_formSwap[pid];
    if ((link->checkWolf() != 0) == wantWolf) {
        fs.active = false;
        return true;
    }
    if (!fs.active) {
        fs.active = true;
        fs.wantWolf = wantWolf;
        // Vanilla metamorphose preamble (loadModelDVD): release the current
        // arc (refcounted — shared arcs stay alive for the real Link), reset
        // the phase machine, free this Link's arc heap, pick the target arc.
        dComIfG_resDelete(&link->mPhaseReq, link->mArcName);
        cPhs_Reset(&link->mPhaseReq);
        link->mpArcHeap->freeAll();
        link->setArcName(wantWolf ? TRUE : FALSE);
    }
    if (dComIfG_resLoad(&link->mPhaseReq, link->mArcName, link->mpArcHeap) != cPhs_COMPLEATE_e) {
        return false;  // multi-frame load — hold the last pose
    }
    // Skeleton swap; changeModelDataDirect* re-arms the shared-model-data
    // mtxCalc pointers, which modelCalc() re-asserts every frame (risk 2).
    if (wantWolf) {
        link->changeWolf();
    } else {
        link->changeLink(0);
    }
    // The old arc was freed, so the anm packs now dangle. Re-point them from
    // the new arc the same way create() does (calc() samples pack 0 and
    // skips NULL packs, so the rest are cleared).
    for (int i = 0; i < 3; ++i) {
        link->mNowAnmPackUnder[i].setAnmTransform(nullptr);
        link->mNowAnmPackUpper[i].setAnmTransform(nullptr);
    }
    J3DAnmTransform* underBck = nullptr;
    J3DAnmTransform* upperBck = nullptr;
    link->getUnderUpperAnime(daAlink_c::ANM_WAIT, &underBck, &upperBck, 0, 0x2C00);
    if (underBck != nullptr) {
        link->mNowAnmPackUnder[0].setAnmTransform(underBck);
    }
    link->mNowAnmPackUpper[0].setAnmTransform(upperBck != nullptr ? upperBck : underBck);
    if (wantWolf) {
        link->setSingleAnimeWolfBase(daAlink_c::WANM_WAIT);
    } else {
        link->setSingleAnimeBase(daAlink_c::ANM_WAIT);
    }
    fs.active = false;
    return true;
}

void ApplyPuppetState(daAlink_c* link) {
    const PlayerId pid = puppetPlayerId(fopAcM_GetID(link));
    if (pid == kInvalidPlayerId) {
        return;
    }
    ReceiveSlot& slot = g_receive[pid];
    if (!slot.hasState) {
        return;  // nothing received yet — hold the spawn pose
    }
    const net::PlayerStateMsg& st = slot.state;

    // Hidden state: the remote player is in another stage or room (M4.5
    // stay-put join — players meet by traveling, so a remote on another stage
    // stays hidden until both share a stage). Skip pose work; draw() returns
    // early for hidden puppets (Anchor's off-scene -9999, done via a draw
    // gate so the framework never sees a far-away actor).
    //
    // Stage is compared via the wire (st.stage), not the session worldStage
    // heuristic: room numbers are not unique across stages — a remote who
    // walked to the spring (F_SP104) or into a house interior (R_SP01) would
    // otherwise stay "in our room" whenever its room number coincides with
    // ours (e.g. both room 1).
    const bool sameStage = std::strcmp(st.stage, LocalStageName()) == 0;
    PuppetEntry& entry = g_puppets[pid];
    entry.hidden = !sameStage || st.roomNo != LocalRoomNo();
    if (entry.hidden) {
        return;
    }

    // Frozen-in-cutscene: while the remote player is inside a demo we hold
    // the last pose (accepted design — the puppet never runs demo code).
    if (st.stateFlags & net::kPlayerStateFlagDemo) {
        return;
    }

    // 1) transform
    link->current.pos.x = st.pos.x;
    link->current.pos.y = st.pos.y;
    link->current.pos.z = st.pos.z;
    link->shape_angle.y = st.yaw;
    link->current.angle.y = st.yaw;
    link->mBodyAngle.x = st.pitch;

    // 2) form flag — swap the skeleton when the received form differs
    //    (changeWolf/changeLink are heavier: the target arc loads on demand
    //    first; the host-save transform-status write inside them is
    //    suppressed for puppets in d_a_alink_wolf.inc).
    const bool wantWolf = st.form != 0;
    if (wantWolf != (link->checkWolf() != 0)) {
        if (!DriveFormSwap(link, pid, wantWolf)) {
            entry.hidden = true;  // model data is freed mid-swap — do not draw
            return;
        }
        // Swap completed this frame; fall through and pose the new skeleton.
    }

    // 3) mProcID — PROC_WAIT v1 (pose replaces the action state machine)
    link->mProcID = daAlink_c::PROC_WAIT;

    // face expression on change (bck/btp indices; the face model calc inside
    // setItemMatrix/setWolfItemMatrix picks them up)
    if (st.faceBckIdx != 0xFFFF && st.faceBckIdx != link->mFaceBckHeap.getIdx()) {
        link->setFaceBck(st.faceBckIdx, FALSE, 0xFFFF);
    }
    if (st.faceBtpIdx != 0xFFFF && st.faceBtpIdx != link->mFaceBtpHeap.getIdx()) {
        link->setFaceBtp(st.faceBtpIdx, FALSE, 0xFFFF);
    }

    // 4) + 5) root transform + anim frame advance
    link->setMatrix();
    link->allAnimePlay();

    // face frame + texture-anime sampling (reproduces the sender's face)
    if (link->field_0x215c != nullptr) {
        link->field_0x215c->setFrame(st.faceFrame);
    } else {
        link->mUnderFrameCtrl[0].setFrame(st.faceFrame);
    }
    link->playFaceTextureAnime();

    // 6) model calc — modelCalc re-asserts this Link's mtxCalc on the shared
    //    J3DModelData joints before calc() (two Links sharing model data
    //    would otherwise render each other's animation).
    link->modelCalc(link->mpLinkModel);

    // pose copy — the interp-aware J3DModel::setAnmMtx API, never raw
    // mtx-buffer writes. Clamp to the local skeleton's joint count (form
    // mismatch protection even if the form event raced the pose).
    const u16 localJoints = link->mpLinkModel->getModelData()->getJointNum();
    const u16 copyCount = st.jointCount < localJoints ? st.jointCount : localJoints;
    for (u16 j = 0; j < copyCount; ++j) {
        Mtx mtx;
        std::memcpy(mtx, st.joints[j], sizeof(Mtx));
        link->mpLinkModel->setAnmMtx(j, mtx);
        link->mpLinkModel->setScaleFlag(j, static_cast<u8>((st.scaleFlags[j >> 3] >> (j & 7)) & 1));
    }
    Mtx baseMtx;
    std::memcpy(baseMtx, st.baseTR, sizeof(Mtx));
    link->mpLinkModel->setBaseTRMtx(baseMtx);

    // MAJOR M1 (puppet render fix): the Link body model is primarily
    // weight-envelope-skinned (133 envelope matrices vs 35 anm matrices;
    // drawFullWgt=23, drawMtxNum=156). calc() computed the envelope matrices
    // from the puppet's OWN anm just above (modelCalc), so without this the
    // rigid joints follow the synced pose while the envelope-weighted
    // vertices (feet, whole back, shoulders) hold the puppet's local pose —
    // "some vertexes are not following the rest of the animation". Recompute
    // the envelopes from the pasted anm matrices (a pure function of
    // mpAnmMtx + inv-joint matrices), and re-record them for frame interp.
    link->mpLinkModel->calcWeightEnvelopeMtx();
#ifdef TARGET_PC
    for (u16 i = 0; i < link->mpLinkModel->getModelData()->getWEvlpMtxNum(); ++i) {
        dusk::frame_interp::record_final_mtx(link->mpLinkModel->getWeightAnmMtx(i));
    }
#endif

    // 7) item/face/hat model attachment at the (synced) item joints
    if (!link->checkWolf()) {
        link->setItemMatrix(0);
    } else {
        link->setWolfItemMatrix();
    }

    // 8) derived presentation positions
    link->setBodyPartPos();
    link->setAttentionPos();

    // 9) colliders at the real pose
    if (!link->checkWolf()) {
        link->setCollisionPos();
    } else {
        link->setWolfCollisionPos();
    }

    // 10) per-frame Tg registration — dCcS clears registrations every frame,
    //     so the frozen puppet re-registers its three target cylinders here
    //     (risk 6). Host hearts stay safe: the puppet's damage paths are
    //     neutralized (setDamagePoint guards), and v1 friendly fire is off.
    for (int i = 0; i < 3; ++i) {
        link->mTgCyls[i].OnTgSetBit();
        dComIfG_Ccsp()->Set(&link->mTgCyls[i]);
        dComIfG_Ccsp()->SetMass(&link->mTgCyls[i], 1);
    }

    // 11) ground + room info
    link->mLinkAcch.CrrPos(dComIfG_Bgsp());
    link->setRoomInfo();

    // Pose-mirroring diagnostics (throttled to 1 Hz per puppet): the applied
    // position vs the received position, for the M1 accept test.
    if (g_frameCount % 60 == 0) {
        CoopLog.info("coop: apply player {} to pos=({:.1f},{:.1f},{:.1f}) recv=({:.1f},{:.1f},{:.1f}) yaw={} room={}",
            pid, link->current.pos.x, link->current.pos.y, link->current.pos.z, st.pos.x, st.pos.y,
            st.pos.z, st.yaw, fopAcM_GetRoomNo(link));
    }

    // reliable-event application (equip)
    ApplyPendingEvent(link, pid);
}

// ---------------------------------------------------------------------------
// Spawn pump
// ---------------------------------------------------------------------------

void PumpSpawns() {
    // Complete / fail / stale tracking.
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        PuppetEntry& e = g_puppets[i];
        if (e.state == SpawnState::Creating) {
            if (fopAcM_SearchByID(e.pid) == nullptr) {
                // Create errored or the process died (e.g. stage change
                // mid-create): back off and re-request.
                e.state = SpawnState::Requested;
                e.pid = fpcM_ERROR_PROCESS_ID_e;
                e.actor = nullptr;
                e.createStartFrame = 0;
                g_createInFlight = false;
                e.retryFrame = g_frameCount + 30;
            } else if (g_frameCount - e.createStartFrame >= kCreateDeadlineFrames) {
                // MAJOR M3: the create is alive but stuck in phase 2 on
                // cPhs_INIT_e — onLinkCreated will never fire and the
                // in-flight lock would block every later spawn (for any
                // player). Delete the stuck process; its destructor
                // (onLinkDestroyed) clears this entry and releases the lock.
                // Count the strike and drop the spawn after too many.
                CoopLog.warn(
                    "coop: puppet {} create stuck {} frames at cPhs_INIT (pid {}); aborting create",
                    i, g_frameCount - e.createStartFrame, e.pid);
                fopAcM_delete(e.pid);
                CreateLimiter& lim = g_createLimiter[i];
                // Capstone MINOR G (review-full-deepseek MINOR G): remember
                // the anchor where this create stuck (real Link pos + room) so
                // the strike budget can be reset when the host moves away
                // materially — the abort cause is position-specific.
                lim.anchorRoom = LocalRoomNo();
                if (g_realLink != nullptr) {
                    lim.anchor[0] = g_realLink->current.pos.x;
                    lim.anchor[1] = g_realLink->current.pos.y;
                    lim.anchor[2] = g_realLink->current.pos.z;
                    lim.hasAnchor = true;
                } else {
                    lim.hasAnchor = false;
                }
                if (++lim.deadlineHits >= kCreateDeadlineStrikes) {
                    lim.dropped = true;
                    CoopLog.warn(
                        "coop: puppet {} create hit its deadline {} times; dropping spawn",
                        i, lim.deadlineHits);
                }
            }
            // else: still multi-phase; onLinkCreated flips it to Active.
        } else if (e.state == SpawnState::Active) {
            if (fopAcM_SearchByID(e.pid) == nullptr) {
                // Stage changed / actor died. If the player is still in the
                // session, re-spawn on the new stage; otherwise drop.
                const bool stillPresent = g_session.roster()[i].present && i != SelfIdChecked();
                e = PuppetEntry{};
                g_createInFlight = false;
                if (stillPresent) {
                    e.state = SpawnState::Requested;
                    CoopLog.info("coop: player {} puppet died; re-requesting", i);
                }
            }
        }
    }

    // Issue at most one create per frame (serialized — the vanilla create
    // path shares a static bgWaitFlg, and multi-phase creates must not
    // interleave; plan Rev 3 R4).
    if (g_createInFlight) {
        return;
    }
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        PuppetEntry& e = g_puppets[i];
        if (e.state != SpawnState::Requested) {
            continue;
        }
        if (e.retryFrame > g_frameCount) {
            continue;
        }
        if (g_createLimiter[i].dropped) {
            // Capstone MINOR G (review-full-deepseek-v4-flash-0731.md MINOR G):
            // the M3 strike budget is position-specific, not permanent — after
            // 3 create-deadline aborts the spawn was dropped forever until
            // leave/rejoin, so a host standing at a cliff edge for 30+ s
            // permanently hid the remote's puppet. Reset the budget when the
            // host's position/room changed materially since the stuck anchor:
            // the next spawn attempt from a clear spot can then succeed.
            CreateLimiter& lim = g_createLimiter[i];
            if (lim.hasAnchor) {
                const bool moved = lim.anchorRoom != LocalRoomNo() || g_realLink == nullptr ||
                                   std::fabs(g_realLink->current.pos.x - lim.anchor[0]) > 300.0f ||
                                   std::fabs(g_realLink->current.pos.y - lim.anchor[1]) > 300.0f ||
                                   std::fabs(g_realLink->current.pos.z - lim.anchor[2]) > 300.0f;
                if (moved) {
                    lim = CreateLimiter{};
                    CoopLog.info(
                        "coop: puppet {} spawn limiter reset (host moved away from the stuck anchor)",
                        i);
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }
        // Wait for the real Link's create to complete (its create and a
        // puppet create must never overlap on the shared bgWaitFlg).
        if (!g_realLinkReady || g_realLink == nullptr) {
            continue;
        }
        const daAlink_c* real = g_realLink;
        const int roomNo = fopAcM_GetRoomNo(real);
        cXyz pos = real->current.pos;
        csXyz angle = real->shape_angle;
        // Small offset in front of the real Link so the two never overlap.
        pos.x += cM_ssin(angle.y) * 120.0f;
        pos.z += cM_scos(angle.y) * 120.0f;

        // Spawn on the real Link's own layer (the stage layer, where the
        // real Link was moved by fopAcM_setStageLayer during its create), so
        // the puppet survives room changes exactly like the real Link. The
        // play scene's layer (fpcLy_ROOT) is not reliably reachable from the
        // pre-execute pump, but the process's layer tag is always valid.
        base_process_class* realProc = reinterpret_cast<base_process_class*>(g_realLink);
        layer_class* savedLayer = fpcLy_CurrentLayer();
        if (realProc->layer_tag.layer == nullptr) {
            continue;  // not queued yet — try next frame
        }
        fpcLy_SetCurrentLayer(realProc->layer_tag.layer);
        const fpc_ProcID pid =
            fopAcM_create(fpcNm_ALINK_e, 0xFFFF, 0, &pos, roomNo, &angle, nullptr, -1, nullptr);
        fpcLy_SetCurrentLayer(savedLayer);

        if (pid == fpcM_ERROR_PROCESS_ID_e) {
            CoopLog.warn("coop: fopAcM_create(ALINK) failed for player {}", i);
            e.retryFrame = g_frameCount + 60;
            continue;
        }
        e.pid = pid;
        e.state = SpawnState::Creating;
        e.createStartFrame = g_frameCount;
        g_createInFlight = true;
        CoopLog.info("coop: puppet spawn requested for player {} (pid {})", i, pid);
        break;  // serialized: one create in flight
    }
}

void PumpSessionAndSpawns() {
    const bool live = SessionLive();
    if (!live) {
        if (g_wasLive) {
            ClearAllPuppets();
            g_receive = {};
            ResetSenderTrackers();
            CoopLog.info("coop: session ended; puppets cleared");
        }
        g_wasLive = false;
        return;
    }
    g_wasLive = true;

    // Roster diff -> spawn requests / despawns.
    const auto& roster = g_session.roster();
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        const bool present = roster[i].present && i != SelfIdChecked();
        PuppetEntry& e = g_puppets[i];
        if (present) {
            if (e.state == SpawnState::None) {
                e.state = SpawnState::Requested;
            }
        } else if (e.state == SpawnState::Creating || e.state == SpawnState::Active) {
            CoopLog.info("coop: player {} left; despawning puppet", i);
            g_createLimiter[i] = CreateLimiter{};  // a rejoin restarts the M3 deadline budget
            if (e.actor != nullptr) {
                fopAcM_delete(e.actor);
            } else if (e.pid != fpcM_ERROR_PROCESS_ID_e) {
                fopAcM_delete(e.pid);
            }
            // Keep the entry flagged (Despawning) until ~daAlink_c clears it,
            // so the destructor's slot-0 guards keep engaging.
            e.state = SpawnState::Despawning;
            e.actor = nullptr;
        }
    }
    PumpSpawns();
}

/// Capstone MINOR F: reset the failed-start state (and re-arm a restart after
/// a terminal client-side end) whenever a relevant net.* var changes. The
/// user edits these from the Settings -> Network tab / cvars to fix a
/// port-busy or join failure; without this the failed start retried the SAME
/// broken values forever.
void RegisterNetVarCallbacks() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    const auto onNetVarChange = [](dusk::config::ConfigVarBase&, const void*) {
        g_startFailed = false;
        // A terminal client-side end (Rejected / Ended) must be re-armable
        // with the new values — tear the old session down so the next
        // EnsureSession starts fresh. An ACTIVE session is left untouched
        // (a mid-game name/port edit does not kill the current game).
        if (g_sessionStarted &&
            (g_session.state() == net::SessionState::Rejected ||
             g_session.state() == net::SessionState::Ended))
        {
            g_session.Stop();
            g_sessionStarted = false;
            g_rejectedNotified = false;
            CoopLog.info("coop: net.* vars changed; re-arming session start");
        }
    };
    dusk::config::subscribe(net::config::enabled.getName(), onNetVarChange);
    dusk::config::subscribe(net::config::hostPort.getName(), onNetVarChange);
    dusk::config::subscribe(net::config::joinHost.getName(), onNetVarChange);
    dusk::config::subscribe(net::config::sessionName.getName(), onNetVarChange);
    dusk::config::subscribe(net::config::role.getName(), onNetVarChange);
}

void EnsureSession() {
    RegisterNetVarCallbacks();
    const bool wantEnabled = net::config::enabled.getValue();
    if (!wantEnabled) {
        if (g_sessionStarted) {
            g_session.Stop();
            g_sessionStarted = false;
            g_startFailed = false;
            CoopLog.info("coop: networking disabled; session stopped");
        }
        return;
    }
    if (g_sessionStarted || g_startFailed) {
        return;
    }
    net::SessionConfig cfg;
    cfg.port = net::config::hostPort.getValue();
    cfg.name = net::config::sessionName.getValue();
    cfg.joinHost = net::config::joinHost.getValue();
    cfg.version = net::kProtocolVersion;
    // M4 (D6): the host fills worldStage_ from the real Link every frame
    // (onGameFrame), so a joiner receives the real stage in JoinAccept /
    // WorldInit — the M1 TODO is now live. The inert seed below is only the
    // pre-Link fallback (stage[0] = '\0' => "host stage unknown").
    cfg.stage.stage[0] = '\0';
    const bool isClient = net::config::role.getValue() == "client";
    bool ok;
    if (isClient) {
        ok = g_session.StartClient(cfg);
    } else {
        ok = g_session.StartHost(cfg);
    }
    if (!ok) {
        const bool first = !g_startFailed;
        g_startFailed = true;
        g_startFailFrame = g_frameCount;
        // Capstone MINOR F: log the failure cause distinctly (port-busy vs
        // already-running vs resolve-failed) and toast ONCE per failure — the
        // old code logged a generic line every 3 s forever.
        const char* reason = g_session.startFailureReason();
        if (reason == nullptr || reason[0] == '\0') {
            reason = "unknown cause";
        }
        CoopLog.error("coop: failed to start {} session ({}); retrying in 3s{}",
            isClient ? "client" : "host", reason, first ? "" : " (retry)");
        if (first) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s: %s",
                isClient ? "Could not start the client session" : "Could not host the session",
                reason);
            NotifyCoop("Co-op session failed to start", buf);
        }
        return;
    }
    g_sessionStarted = true;
    g_session.SetGameMessageHandler(OnGameMessage);
    CoopLog.info("coop: {} session started (port {})", isClient ? "client" : "host",
        g_session.boundPort());
}

// ---------------------------------------------------------------------------
// M4 helpers — toasts, host-leave UX, discovery
// ---------------------------------------------------------------------------

/// Pushes a HUD toast via the project's toast mechanism (dusk::ui, the
/// Aurora/RmlUi overlay — the same queue autosave/achievements use).
void NotifyCoop(const char* title, const char* content) {
    dusk::ui::push_toast({
        .type = "coop",
        .title = title,
        .content = content,
        .duration = std::chrono::milliseconds(5000),
    });
}

/// Host-leave UX (D8): the frame the session drops, freeze+despawn the
/// puppets (PumpSessionAndSpawns already does the despawn + state reset) and
/// tell the player what happened via a toast + log. The game continues as
/// vanilla single-player.
void NoticeSessionEnd(bool justEnded) {
    if (!justEnded) {
        return;
    }
    if (hostRole() || !g_sessionStarted) {
        return;  // the host ending its own session (Stop/shutdown) needs no notice
    }
    switch (g_session.endReason()) {
    case net::SessionEndReason::HostLeft:
        CoopLog.warn("coop: host ended the session — returning to single-player");
        NotifyCoop("Host left", "The host ended the session. Remote players have gone home; "
                                 "your game continues as single-player.");
        break;
    case net::SessionEndReason::ConnectionLost:
        CoopLog.warn("coop: connection to the host lost — returning to single-player");
        NotifyCoop("Host disconnected", "Lost the connection to the host. Remote players have "
                                         "gone home; your game continues as single-player.");
        break;
    default:
        break;  // kicked / shutdown: no toast (or covered elsewhere)
    }
}

/// LAN discovery lifecycle: the host announces its session (session name /
/// players / port); clients listen and log what they find. Manual net.joinHost
/// stays the fallback; the settings UI lists discovered sessions.
void DriveDiscovery() {
    const bool hostUp = g_sessionStarted && hostRole() &&
                        g_session.state() == net::SessionState::Listening;
    if (hostUp && !g_announcerActive) {
        g_announcerActive = true;
        // M4.5 (review MINOR 6): seed the player count BEFORE the thread
        // starts — ThreadMain's very first datagram already reads players_, so
        // SetPlayers-after-Start advertised 0 players for up to one announce
        // interval. (Everything sequenced before the thread is created
        // happens-before the thread's first read.)
        g_discoveryAnnouncer.SetPlayers(static_cast<u8>(remoteCount() + 1));
        g_discoveryAnnouncer.Start(net::config::sessionName.getValue(), g_session.boundPort(),
            net::kMaxLocalPlayers);
    }
    if (g_announcerActive) {
        if (!hostUp) {
            g_announcerActive = false;
            g_discoveryAnnouncer.Stop();
        } else {
            g_discoveryAnnouncer.SetPlayers(static_cast<u8>(remoteCount() + 1));
        }
    }

    const bool clientUp = g_sessionStarted && !hostRole() &&
                          g_session.state() == net::SessionState::Joined;
    if (clientUp && !g_listenerActive) {
        g_listenerActive = true;
        dusk::net::discovery::SetActiveListener(&g_discoveryListener);
        g_discoveryListener.Start();
    }
    if (g_listenerActive) {
        if (!clientUp) {
            g_listenerActive = false;
            dusk::net::discovery::SetActiveListener(nullptr);
            g_discoveryListener.Stop();
        }
        // New sessions are logged once each by the listener itself
        // ("discovery: found session ...") and listed in Settings -> Network;
        // nothing to do here beyond keeping the listener alive.
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// sessionActive()/hostRole() are exported (coop.h) but currently have no
// callers outside this TU (review m1 MINOR m5). They are retained for the M4
// host-leave UX / LAN-discovery UI; keeping them behind the same
// TARGET_PC-guarded vanilla attachment points costs nothing and avoids
// churn. selfId()/remoteCount() ARE used (sender + modelCalc gate).

bool sessionActive() {
    return SessionLive();
}

bool hostRole() {
    return g_sessionStarted && g_session.role() == net::SessionRole::Host;
}

net::PlayerId selfId() {
    return SelfIdChecked();
}

int remoteCount() {
    if (!SessionLive()) {
        return 0;
    }
    int count = 0;
    const auto& roster = g_session.roster();
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        if (roster[i].present && i != SelfIdChecked()) {
            ++count;
        }
    }
    return count;
}

bool isPuppet(const daAlink_c* link) {
    if (link == nullptr) {
        return false;
    }
    return EntryForPid(fopAcM_GetID(link)) != nullptr;
}

net::PlayerId puppetPlayerId(fpc_ProcID pid) {
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        const PuppetEntry& e = g_puppets[i];
        if (e.pid == pid && (e.state == SpawnState::Creating || e.state == SpawnState::Active ||
                             e.state == SpawnState::Despawning))
        {
            return i;
        }
    }
    return kInvalidPlayerId;
}

fopAc_ac_c* puppetActorFor(net::PlayerId playerId) {
    if (playerId >= kMaxLocalPlayers) {
        return nullptr;
    }
    const PuppetEntry& e = g_puppets[playerId];
    if (e.state != SpawnState::Active || e.actor == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<fopAc_ac_c*>(e.actor);
}

void onLinkCreated(daAlink_c* link) {
    if (isPuppet(link)) {
        const PlayerId pid = puppetPlayerId(fopAcM_GetID(link));
        if (pid != kInvalidPlayerId) {
            PuppetEntry& e = g_puppets[pid];
            e.actor = link;
            e.state = SpawnState::Active;
            e.hidden = true;  // until the first received state marks the room
            g_createInFlight = false;
            g_createLimiter[pid] = CreateLimiter{};  // success clears M3 strike count
            CoopLog.info("coop: puppet for player {} active (pid {})", pid, e.pid);
        }
        return;
    }
    g_realLink = link;
    g_realLinkPid = fopAcM_GetID(link);
    g_realLinkReady = true;
    ResetSenderTrackers();
    CoopLog.info("coop: real Link registered (pid {})", g_realLinkPid);
}

void onLinkDestroyed(daAlink_c* link) {
    const fpc_ProcID pid = fopAcM_GetID(link);
    if (isPuppet(link)) {
        const PlayerId playerId = puppetPlayerId(pid);
        if (playerId != kInvalidPlayerId) {
            CoopLog.info("coop: puppet for player {} destroyed (pid {})", playerId, pid);
            g_puppets[playerId] = PuppetEntry{};
            g_createInFlight = false;
        }
        return;
    }
    if (g_realLink == link) {
        g_realLink = nullptr;
        g_realLinkPid = fpcM_ERROR_PROCESS_ID_e;
        g_realLinkReady = false;
        CoopLog.info("coop: real Link destroyed");
    }
}

int puppetExecute(daAlink_c* link) {
    ApplyPuppetState(link);
    return 1;
}

void sendPlayerState(daAlink_c* link) {
    if (link == nullptr || !SessionLive() || SelfIdChecked() >= kMaxLocalPlayers) {
        return;
    }
    // Capstone MINOR 6 (review-full-glm-5.2.md MINOR 6): defense-in-depth —
    // the puppet execute path returns before this call today, but the sender
    // must not rely on that ordering (a puppet's pose is never "ours" to
    // send; the puppet apply path owns it).
    if (isPuppet(link)) {
        return;
    }
    // MAJOR M2: the room-change event is sent regardless of the sender gate.
    // Two players entering the same new room together both hold the other's
    // stale room; the gate (which now also opens on "my room changed since
    // their last state") un-sticks this frame, and the reliable SceneChange
    // below is the prompt that makes the receiving side's gate open as soon
    // as the matching PlayerState lands. Reliable + tiny, so the cost of
    // sending it while alone is nil.
    const s8 roomNow = fopAcM_GetRoomNo(link);
    const char* myStage = LocalStageName();
    // MAJOR M1 (stage/room change propagation): the reliable SceneChange
    // event + a short send window fire whenever the STAGE or ROOM changed.
    // Room-only detection misses a stage change whose room number coincides
    // (F_SP103 room 1 -> F_SP104 room 1), and the sender gate's "we moved"
    // check closes after one inbound reply — earlier than the first post-move
    // PlayerState may land. Without the window a dropped state left the
    // remote's slot on the old stage: a puppet frozen at the exit spot.
    // M4.5 (review MINOR 1): the event marks whether the move is WITHIN the
    // current stage (scene=1) or a stage change (scene=0) — the host's
    // ownership-table sniff keys (last-known stage, new room) which is only
    // valid for same-stage moves; a cross-stage SceneChange must be left to
    // the first new-stage PlayerState (channel 1, inside this send window).
    const bool stageChanged = std::strcmp(myStage, g_lastSentStage) != 0;
    if (roomNow != g_lastSentRoom || stageChanged) {
        g_lastSentRoom = roomNow;
        std::snprintf(g_lastSentStage, sizeof(g_lastSentStage), "%s", myStage);
        g_postChangeSendWindow = 30;
        SendPlayerEvent(net::PlayerEventId::SceneChange,
            static_cast<u32>(static_cast<s32>(roomNow)), 0,
            /*sameStage=*/stageChanged ? 0u : 1u);
    }
    if (g_postChangeSendWindow > 0) {
        --g_postChangeSendWindow;  // keep sending through the post-move window
    } else if (!RemoteInOurRoom(myStage, roomNow)) {
        return;
    }

    net::PlayerStateMsg st = {};
    st.playerId = SelfIdChecked();
    st.roomNo = fopAcM_GetRoomNo(link);
    std::snprintf(st.stage, sizeof(st.stage), "%s", LocalStageName());
    st.form = link->checkWolf() ? 1 : 0;
    st.stateFlags = 0;
    if (link->mRideStatus != 0) {
        st.stateFlags |= net::kPlayerStateFlagRiding;
    }
    if (link->mDamageTimer > 0) {
        st.stateFlags |= net::kPlayerStateFlagInvuln;
    }
    if (link->mProcID == daAlink_c::PROC_SUBJECTIVITY) {
        st.stateFlags |= net::kPlayerStateFlagSubjectivity;
    }
    if (link->checkDeadHP()) {
        st.stateFlags |= net::kPlayerStateFlagDowned;
    }
    if (link->mDemo.getDemoType() != 0) {
        st.stateFlags |= net::kPlayerStateFlagDemo;
    }
    const u16 jointCount = link->mpLinkModel->getModelData()->getJointNum();
    st.jointCount = jointCount < net::kMaxJoints ? jointCount : net::kMaxJoints;
    st.yaw = link->shape_angle.y;
    st.pitch = link->mBodyAngle.x;
    st.faceBckIdx = link->mFaceBckHeap.getIdx();
    st.faceBtpIdx = link->mFaceBtpHeap.getIdx();
    st.faceFrame = static_cast<s16>(link->field_0x215c != nullptr
                                        ? link->field_0x215c->getFrame()
                                        : link->mUnderFrameCtrl[0].getFrame());
    st.pos.x = link->current.pos.x;
    st.pos.y = link->current.pos.y;
    st.pos.z = link->current.pos.z;
    std::memcpy(st.baseTR, link->mpLinkModel->getBaseTRMtx(), sizeof(Mtx));
    for (u16 j = 0; j < st.jointCount; ++j) {
        std::memcpy(st.joints[j], link->mpLinkModel->getAnmMtx(j), sizeof(Mtx));
        if (link->mpLinkModel->getMtxBuffer()->getScaleFlag(j) != 0) {
            st.scaleFlags[j >> 3] |= static_cast<u8>(1 << (j & 7));
        }
    }

    net::PayloadUnion payload = {};
    payload.playerState = st;
    g_session.SendGameMessage(net::MsgType::PlayerState, payload);

    // Pose-mirroring diagnostics (throttled to 1 Hz): the sender's frame-
    // final position + root matrix + a sample joint, for the M1 accept test.
    if (g_frameCount % 60 == 0) {
        CoopLog.info("coop: send player {} pos=({:.1f},{:.1f},{:.1f}) yaw={} j0=({:.3f},{:.3f},{:.3f},{:.3f})",
            st.playerId, st.pos.x, st.pos.y, st.pos.z, st.yaw, st.joints[0][0][3], st.joints[0][1][3],
            st.joints[0][2][3], st.baseTR[2][3]);
    }

    SendEventsOnChange(link);
}

bool puppetDrawHidden(const daAlink_c* link) {
    const PlayerId pid = puppetPlayerId(fopAcM_GetID(link));
    if (pid == kInvalidPlayerId) {
        return false;
    }
    return g_puppets[pid].hidden;
}

void onGameFrame() {
    ++g_frameCount;
    EnsureSession();
    // Capstone MINOR F: the client's join rejection gets a ONE-SHOT toast
    // (version mismatch / session full / invalid slot / join timeout) —
    // previously only a log line, leaving the client stuck until the user
    // toggled net.enabled (which now also resets via any net.* var change).
    if (g_sessionStarted && g_session.state() == net::SessionState::Rejected) {
        if (!g_rejectedNotified) {
            g_rejectedNotified = true;
            CoopLog.warn("coop: join rejected: {}", g_session.rejectReasonName());
            NotifyCoop("Join rejected", g_session.rejectReasonName());
        }
    } else {
        g_rejectedNotified = false;
    }
    // Capture liveness BEFORE Update() drains the inbox: a SessionEnd / host
    // disconnect arriving this frame flips the state to Ended inside Update,
    // so a pre-Update capture is the only way to detect the live->dead
    // transition for the host-leave UX (M4 D8).
    const bool wasLive = SessionLive();
    if (g_sessionStarted) {
        // Retry a failed start after a backoff.
        if (g_startFailed && g_frameCount - g_startFailFrame >= 180) {
            g_startFailed = false;
        }
        g_session.Update();
        // Capstone MAJOR 1 (glue half — the review's "alternatively"): a
        // session that ended from the host's side (SessionEnd / connection
        // loss) reaches Ended with the transport STILL running. Stop() now
        // always tears the transport down; call it the frame we observe Ended
        // so the socket thread is released promptly. g_sessionStarted stays
        // set, so the session is NOT auto-restarted here — the user re-arms a
        // new session via net.enabled / the Network tab (or a net.* var
        // change, which resets the failed-start state, MINOR F).
        if (g_session.state() == net::SessionState::Ended && g_session.transportRunning()) {
            g_session.Stop();
            CoopLog.info("coop: session ended remotely; transport torn down (a new session can start)");
        }
    }
    PumpSessionAndSpawns();
    // M4 (D6 revised — M4.5): the host fills worldStage_ from the real Link
    // every frame — the M1 TODO ('cfg.stage was inert with stage[0]=0') — so a
    // mid-game joiner is told where the host's world is (JoinAccept/WorldInit
    // stage carry). Join-warp is REMOVED (stay-put): the client boots into its
    // own save stage and players meet by traveling; the carry behind the
    // cross-stage `stageOk` puppet-visibility gate (a puppet's visibility keys
    // on the remote's REAL stage from PlayerState, so it stays hidden until
    // both players share a stage). The host's own room feeds the
    // room-ownership table (host-default-owns).
    if (g_sessionStarted && hostRole() && g_realLinkReady && g_realLink != nullptr) {
        net::StageInfo st = {};
        std::snprintf(st.stage, sizeof(st.stage), "%s", LocalStageName());
        st.room = fopAcM_GetRoomNo(g_realLink);
        st.layer = dComIfGp_getStartStageLayer();
        st.point = dComIfGp_getStartStagePoint();
        g_session.setWorldStage(st);
        g_session.setLocalRoom(LocalStageName(), fopAcM_GetRoomNo(g_realLink));
    }
    // M4: host-leave UX + LAN discovery.
    NoticeSessionEnd(wasLive && !SessionLive());
    DriveDiscovery();
    // M2: enemy authority — host registration/snapshots/deaths, client
    // freeze/apply state, room-clear. No-op when the session is not live.
    dusk::coop::enemy::onGameFrame();
    // M3: time of day & weather — host publisher (TimeSync 1 Hz / TimeEvent /
    // WeatherChange + world info for joiners), client world-state re-seed.
    // The actual client replica/force hooks live in d_kankyo.cpp /
    // d_a_kytag06.cpp under TARGET_PC. No-op when the session is not live.
    dusk::coop::timeweather::onGameFrame();
}

void shutdown() {
    // Graceful session teardown on game exit: a client sends PlayerLeave, the
    // host broadcasts SessionEnd (00-network.md §4), then the transport is
    // stopped and joined. An abrupt process kill (no chance to run this) is a
    // transport-level timeout limitation — ENet only detects a dead peer via
    // an unacknowledged reliable command.
    if (g_sessionStarted) {
        g_session.Stop();
        g_sessionStarted = false;
        g_startFailed = false;
        CoopLog.info("coop: session stopped on shutdown");
    }
    // M4: stop the discovery threads (they hold sockets; a leaked announcer
    // would keep broadcasting after shutdown).
    if (g_announcerActive) {
        g_announcerActive = false;
        g_discoveryAnnouncer.Stop();
    }
    if (g_listenerActive) {
        g_listenerActive = false;
        g_discoveryListener.Stop();
    }
    // M2: clear per-stage enemy state (registry, receive slots, room-clear).
    dusk::coop::enemy::shutdown();
    // M3: clear host/clients time-weather module state.
    dusk::coop::timeweather::shutdown();
}

bool sendGameMessage(net::MsgType type, const net::PayloadUnion& payload) {
    if (!g_sessionStarted) {
        return false;
    }
    return g_session.SendGameMessage(type, payload);
}

bool rosterPresent(net::PlayerId pid) {
    if (pid >= kMaxLocalPlayers || !g_sessionStarted) {
        return false;
    }
    return g_session.roster()[pid].present;
}

s8 localRoomNo() {
    return LocalRoomNo();
}

const char* localStageName() {
    return LocalStageName();
}

void setWorldTime(const net::TimeStateInfo& time) {
    g_session.setWorldTime(time);
}

void setWorldWeather(const net::WeatherStateInfo& weather) {
    g_session.setWorldWeather(weather);
}

const net::TimeStateInfo& worldTime() {
    return g_session.worldTime();
}

const net::WeatherStateInfo& worldWeather() {
    return g_session.worldWeather();
}

bool amIRoomOwner(s8 roomNo) {
    if (!SessionLive() || roomNo < 0) {
        return false;
    }
    // M4 room ownership (network.md §6): the room key is (stage, room) — a
    // room number alone is not unique across stages. The session holds the
    // authoritative table (host) or the last RoomOwnershipMsg view (client).
    return g_session.roomOwner(LocalStageName(), roomNo) == SelfIdChecked();
}

bool remoteInRoom(s8 roomNo) {
    if (!SessionLive()) {
        return false;
    }
    // Capstone MINOR L: consolidated sender gate — same implementation as the
    // player sender's RemoteInOurRoom, keyed on our current stage + room. The
    // enemy snapshot sender (owner side) streams to remotes actually in the
    // room it owns; the receive side gates on the local room as well.
    return RemoteInOurRoom(LocalStageName(), roomNo);
}

}  // namespace dusk::coop
