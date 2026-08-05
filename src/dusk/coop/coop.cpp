#include "dusk/coop/coop.h"

#include "dusk/coop/coop_combat.h"
#include "dusk/coop/coop_enemy.h"
#include "dusk/coop/coop_time.h"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
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
    // This machine's room when the remote last sent us PlayerState. Used by
    // the sender gate (MAJOR M2): a remote's stale room can gate us silent
    // forever; if OUR room changed since their last send, the gate opens so
    // they learn our new room.
    s8 myRoomAtLastRecv = -1;
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
u32 g_frameCount = 0;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

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
    } else if (type == net::MsgType::PlayerEvent) {
        const auto& ev = payload.playerEvent;
        if (ev.playerId >= kMaxLocalPlayers || ev.playerId == SelfIdChecked()) {
            return;
        }
        ReceiveSlot& slot = g_receive[ev.playerId];
        slot.event = ev;
        slot.hasEvent = true;
    } else if (type == net::MsgType::EnemySnapshot || type == net::MsgType::EnemyEvent) {
        // M2: enemy authority traffic (freeze/apply + drops/room-clear).
        dusk::coop::enemy::onGameMessage(type, payload);
    } else if (type == net::MsgType::CombatIntent) {
        // M2: combat intents are validated by the sim owner (host role).
        dusk::coop::combat::onGameMessage(type, payload);
    } else if (type == net::MsgType::CombatResult) {
        // M2: result ack — the authoritative HP always rides the next
        // EnemySnapshot; nothing to apply client-side in v1.
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

void SendPlayerEvent(net::PlayerEventId eventId, u32 data, u32 data2) {
    net::PayloadUnion payload = {};
    payload.playerEvent.playerId = SelfIdChecked();
    payload.playerEvent.eventId = static_cast<u8>(eventId);
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
/// in (or unknown to be outside) our room — a client only renders peers in
/// its own scene/room, so same-room peers are the only ones that can see us.
///
/// MAJOR M2 (mutual room-change deadlock): two players entering the same new
/// room together hold each other's stale room, so `state.roomNo == myRoom`
/// is false on both sides and both gates would stay shut forever (both
/// puppets hidden). The gate therefore also opens when OUR room changed
/// since the remote last sent us state — they cannot know where we are, so
/// we send; one PlayerState with the new roomNo re-opens their gate.
bool RemoteInOurRoom(const daAlink_c* link) {
    const s8 myRoom = fopAcM_GetRoomNo(link);
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        if (i == SelfIdChecked()) {
            continue;
        }
        if (!g_session.roster()[i].present) {
            continue;
        }
        const ReceiveSlot& slot = g_receive[i];
        if (!slot.hasState || slot.state.roomNo == myRoom) {
            return true;
        }
        if (slot.myRoomAtLastRecv != myRoom) {
            return true;  // our room changed since their last send
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
        // the M2 deadlock-breaker: it is sent on room change before the
        // sender gate, but the receiving side's room actually rides in
        // PlayerState.roomNo (updated when the matching PlayerState lands) —
        // the event itself is not consumed for rendering.
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

    // Hidden state: the remote player is in another room (or, v1, a stage we
    // are not on — join-warp lands in M4). Skip pose work; draw() returns
    // early for hidden puppets (Anchor's off-scene -9999, done via draw gate
    // so the framework never sees a far-away actor).
    const bool stageOk = strcmp(dComIfGp_getStartStageName(), g_session.worldStage().stage) == 0 ||
                         g_session.worldStage().stage[0] == '\0';
    PuppetEntry& entry = g_puppets[pid];
    entry.hidden = !stageOk || st.roomNo != LocalRoomNo();
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
            continue;  // M3: spawn was dropped after repeated deadline aborts
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

void EnsureSession() {
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
    cfg.stage.stage[0] = '\0';  // filled from the sim on WorldInit in M4 join-warp
    const bool isClient = net::config::role.getValue() == "client";
    bool ok;
    if (isClient) {
        ok = g_session.StartClient(cfg);
    } else {
        ok = g_session.StartHost(cfg);
    }
    if (!ok) {
        g_startFailed = true;
        g_startFailFrame = g_frameCount;
        CoopLog.error("coop: failed to start {} session; retrying in 3s",
            isClient ? "client" : "host");
        return;
    }
    g_sessionStarted = true;
    g_session.SetGameMessageHandler(OnGameMessage);
    CoopLog.info("coop: {} session started (port {})", isClient ? "client" : "host",
        g_session.boundPort());
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
    // MAJOR M2: the room-change event is sent regardless of the sender gate.
    // Two players entering the same new room together both hold the other's
    // stale room; the gate (which now also opens on "my room changed since
    // their last state") un-sticks this frame, and the reliable SceneChange
    // below is the prompt that makes the receiving side's gate open as soon
    // as the matching PlayerState lands. Reliable + tiny, so the cost of
    // sending it while alone is nil.
    const s8 roomNow = fopAcM_GetRoomNo(link);
    if (roomNow != g_lastSentRoom) {
        g_lastSentRoom = roomNow;
        SendPlayerEvent(net::PlayerEventId::SceneChange,
            static_cast<u32>(static_cast<s32>(roomNow)), 0);
    }
    if (!RemoteInOurRoom(link)) {
        return;
    }

    net::PlayerStateMsg st = {};
    st.playerId = SelfIdChecked();
    st.roomNo = fopAcM_GetRoomNo(link);
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
    if (g_sessionStarted) {
        // Retry a failed start after a backoff.
        if (g_startFailed && g_frameCount - g_startFailFrame >= 180) {
            g_startFailed = false;
        }
        g_session.Update();
    }
    PumpSessionAndSpawns();
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

}  // namespace dusk::coop
