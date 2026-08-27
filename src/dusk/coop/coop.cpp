#include "dusk/coop/coop.h"

#include "dusk/frame_interpolation.h"

#include "dusk/ui/ui.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_horse.h"
#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "d/d_com_inf_game.h"
#include "dusk/config.hpp"
#include "dusk/main.h"
#include "dusk/net/config.h"
#include "dusk/net/local_ipv4.h"
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
#include <string>

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
bool g_startFailNotified = false;
// Host/Connect button: stop this frame, start the next so PumpSessionAndSpawns
// observes !live and clears puppets (a same-frame Stop+Start would skip that).
bool g_restartRequested = false;
// Capstone MINOR F (review-full-deepseek-v4-flash-0731.md MINOR F): one-shot
// notification latch — a join rejection / start failure toasts once per
// occurrence instead of spamming the 3 s retry loop.
bool g_rejectedNotified = false;
// This-process session intent. Host/Connect set it; Disconnect clears it.
// autoConnect / --cvar net.connected / legacy --cvar net.enabled arm it on
// the first EnsureSession of the process. Not written to config.json.
bool g_connected = false;
// autoConnect launch: first session this process is a client (Role ignored).
bool g_forceClient = false;
bool g_sawFirstEnsure = false;

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
/// PlayerId of the puppet whose daAlink_c::create OR daHorse_c::create is in
/// flight, or kInvalidPlayerId. Horse creates share the one-in-flight lock
/// (vanilla create paths must not interleave).
PlayerId g_creatingPid = kInvalidPlayerId;
/// True when g_creatingPid is a horse create rather than a Link create.
bool g_creatingHorse = false;

struct HorseEntry {
    SpawnState state = SpawnState::None;
    fpc_ProcID pid = fpcM_ERROR_PROCESS_ID_e;
    daHorse_c* actor = nullptr;
    bool hidden = false;
    u32 retryFrame = 0;
    u32 createStartFrame = 0;
};
std::array<HorseEntry, kMaxLocalPlayers> g_horses{};

bool CreateInFlight() { return g_creatingPid != kInvalidPlayerId; }

void ClearCreatingIf(PlayerId pid) {
    if (g_creatingPid != pid) {
        return;
    }
    g_creatingPid = kInvalidPlayerId;
    g_creatingHorse = false;
    daAlink_c::clearCreateBgWait();
}

void ClearCreatingAll() {
    if (g_creatingPid == kInvalidPlayerId) {
        return;
    }
    g_creatingPid = kInvalidPlayerId;
    g_creatingHorse = false;
    daAlink_c::clearCreateBgWait();
}

// MAJOR M3: daAlink_c::create() phase 2 can spin on cPhs_INIT_e forever
// (ground check over a cliff at the +120-unit spawn offset, a residual
// ride/portal wait) without the process dying, so onLinkCreated never fires
// and g_creatingPid would block every later spawn for ANY player. The
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
    char anchorStage[net::kMaxStageNameLength] = {};
};
std::array<CreateLimiter, kMaxLocalPlayers> g_createLimiter{};
std::array<CreateLimiter, kMaxLocalPlayers> g_horseLimiter{};

// ---------------------------------------------------------------------------
// Receive slots — latest PlayerState / PlayerEvent per remote player.
// ---------------------------------------------------------------------------

struct ReceiveSlot {
    bool hasState = false;
    net::PlayerStateMsg state{};
    bool hasHorseState = false;
    net::HorseStateMsg horse{};
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
// Appearance state — a puppet's create loads the local save's body/shield
// arcs. When the remote player's form, clothes, or shield differ, the target
// arc is loaded on demand (resDelete + cPhs_Reset + freeAll + setArcName /
// setShieldArcName + resLoad) and changeWolf()/changeLink(0)/setShieldModel
// rebuild the instance. Sword models already live on the Alink arc, so a
// sword change is an instant setSelectEquipItem. 0 on a want/loaded slot
// means "unknown / not yet applied"; dItemNo_NONE_e (0xFF) is a real value.
// ---------------------------------------------------------------------------

struct AppearanceState {
    bool arcLoadActive = false;
    bool shieldLoadActive = false;
    bool wantWolf = false;
    bool loadWantWolf = false;  // form snapshotted when the in-flight body load started
    u8 wantClothes = 0;
    u8 wantSword = 0;
    u8 wantShield = 0;
    u8 loadedClothes = 0;
    u8 loadedSword = 0;
    u8 loadedShield = 0;
    // Latest Equip (applied immediately on receive — not queued behind
    // SceneChange/AttentionChange, which used to drop appearance).
    bool hasEquip = false;
    u16 wantEquip = 0;
    u8 wantSelectItem = 0;
    u16 wantLeftJnt = 0xFFFF;
    u16 wantRightJnt = 0xFFFF;
};

std::array<AppearanceState, kMaxLocalPlayers> g_appearance{};

/// Temporarily overwrite the SAVE select-equip slots so vanilla Link helpers
/// (setArcName / changeLink / setSelectEquipItem / setShieldArcName) read the
/// remote player's clothes/sword/shield. Restores on every exit path — must
/// not span a resLoad yield. Does NOT call dComIfGs_setSelectEquipSword/Shield
/// (those also set collect flags).
class ScopedSelectEquip {
    u8 clothes_;
    u8 sword_;
    u8 shield_;

    static dSv_player_status_a_c& Status() {
        return g_dComIfG_gameInfo.info.getPlayer().getPlayerStatusA();
    }

public:
    ScopedSelectEquip(u8 clothes, u8 sword, u8 shield)
        : clothes_(dComIfGs_getSelectEquipClothes()),
          sword_(dComIfGs_getSelectEquipSword()),
          shield_(dComIfGs_getSelectEquipShield()) {
        auto& status = Status();
        if (clothes != 0) {
            status.setSelectEquip(COLLECT_CLOTHING, clothes);
        }
        if (sword != 0) {
            status.setSelectEquip(COLLECT_SWORD, sword);
        }
        if (shield != 0) {
            status.setSelectEquip(COLLECT_SHIELD, shield);
        }
    }

    ~ScopedSelectEquip() {
        auto& status = Status();
        status.setSelectEquip(COLLECT_CLOTHING, clothes_);
        status.setSelectEquip(COLLECT_SWORD, sword_);
        status.setSelectEquip(COLLECT_SHIELD, shield_);
    }

    ScopedSelectEquip(const ScopedSelectEquip&) = delete;
    ScopedSelectEquip& operator=(const ScopedSelectEquip&) = delete;
};

// ---------------------------------------------------------------------------
// Sender change tracking (reliable PlayerEvent on change only).
// ---------------------------------------------------------------------------

u16 g_lastSentEquip = 0xFFFF;
u8 g_lastSentSelectItem = 0xFF;
u8 g_lastSentClothes = 0xFF;
u8 g_lastSentSword = 0xFF;
u8 g_lastSentShield = 0xFF;
u16 g_lastSentLeftJnt = 0xFFFF;
u16 g_lastSentRightJnt = 0xFFFF;
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
/// Bitmask of roster-present remotes. A newly present bit forces Equip/Form
/// to resend so joiners and rejoiners are not stuck in the local save's tunic.
u8 g_lastRosterMask = 0;

// ---------------------------------------------------------------------------
// M4 state — host-leave UX (D8).
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Forward declaration: defined in the M4 helpers section below (used by
// EnsureSession's capstone MINOR F start-failure toast).
void NotifyCoop(const char* title, const char* content);
void ClearRemoteSlot(PlayerId pid);
bool PoseIsFinite(const net::Vec3f& pos, const Mtx& baseTR, const Mtx* joints, u8 jointCount);

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

std::string TrimCopy(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

void AppendRosterNames(char* buf, size_t cap, size_t& n) {
    if (cap == 0) {
        return;
    }
    if (n >= cap) {
        n = cap - 1;
        buf[n] = '\0';
        return;
    }
    const auto& roster = g_session.roster();
    bool first = true;
    for (u8 i = 0; i < kMaxLocalPlayers && n + 1 < cap; ++i) {
        if (!roster[i].present) {
            continue;
        }
        const char* name = roster[i].name[0] != '\0' ? roster[i].name : "?";
        const int wrote =
            std::snprintf(buf + n, cap - n, "%s%s", first ? "" : ", ", name);
        if (wrote < 0) {
            break;
        }
        n += static_cast<size_t>(wrote);
        if (n >= cap) {
            n = cap - 1;
            break;
        }
        first = false;
    }
}

u32 DeadlineRemainSec() {
    const u64 remainMs = g_session.deadlineRemainMs();
    if (remainMs == 0) {
        return 0;
    }
    const u32 sec = static_cast<u32>((remainMs + 999) / 1000);
    return sec == 0 ? 1 : sec;
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

const HorseEntry* EntryForHorsePid(fpc_ProcID pid) {
    if (pid == fpcM_ERROR_PROCESS_ID_e) {
        return nullptr;
    }
    for (const auto& e : g_horses) {
        if (e.pid == pid && (e.state == SpawnState::Creating || e.state == SpawnState::Active ||
                             e.state == SpawnState::Despawning))
        {
            return &e;
        }
    }
    return nullptr;
}

PlayerId HorsePlayerId(fpc_ProcID pid) {
    if (pid == fpcM_ERROR_PROCESS_ID_e) {
        return kInvalidPlayerId;
    }
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        const HorseEntry& e = g_horses[i];
        if (e.pid == pid && (e.state == SpawnState::Creating || e.state == SpawnState::Active ||
                             e.state == SpawnState::Despawning))
        {
            return i;
        }
    }
    return kInvalidPlayerId;
}

void DisableHorseColliders(daHorse_c* horse) {
    if (horse == nullptr) {
        return;
    }
    for (int i = 0; i < 3; ++i) {
        horse->m_tgco_cyl[i].OffTgSetBit();
        horse->m_tgco_cyl[i].OffCoSetBit();
    }
    horse->m_boar_cyl.OffTgSetBit();
    horse->m_boar_cyl.OffCoSetBit();
    horse->m_at_cyl.OffAtSetBit();
    horse->m_at_cyl.OffCoSetBit();
    horse->m_head_sph.OffTgSetBit();
    horse->m_head_sph.OffCoSetBit();
}

void ReassertRealHorseMtxCalc() {
    daHorse_c* real = dComIfGp_getHorseActor();
    if (real != nullptr && real->m_modelData != nullptr && real->m_mtxcalc != nullptr) {
        real->m_modelData->getJointNodePointer(0)->setMtxCalc(real->m_mtxcalc);
    }
}

void RequestHorseDespawn(HorseEntry& h) {
    if (h.state == SpawnState::Creating || h.state == SpawnState::Active) {
        if (h.actor != nullptr) {
            fopAcM_delete(h.actor);
        } else if (h.pid != fpcM_ERROR_PROCESS_ID_e) {
            fopAcM_delete(h.pid);
        }
        h.state = SpawnState::Despawning;
        h.actor = nullptr;
    } else if (h.state == SpawnState::Requested) {
        h = HorseEntry{};
    }
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

void ClearRemoteSlot(PlayerId pid) {
    if (pid >= kMaxLocalPlayers) {
        return;
    }
    g_receive[pid] = {};
    g_appearance[pid] = {};
    g_createLimiter[pid] = {};
    g_horseLimiter[pid] = {};
}

/// True when a dropped spawn limiter should stay dropped. Resets the
/// strike budget when the host has moved away from the stuck anchor.
bool LimiterStillDropped(CreateLimiter& lim) {
    if (!lim.dropped) {
        return false;
    }
    const char* stageNow = LocalStageName();
    const bool stageMoved =
        lim.anchorStage[0] != '\0' && std::strcmp(lim.anchorStage, stageNow) != 0;
    if (!lim.hasAnchor) {
        if (!g_realLinkReady || g_realLink == nullptr) {
            return true;
        }
        lim = CreateLimiter{};
        return false;
    }
    const bool moved = stageMoved || lim.anchorRoom != LocalRoomNo() || g_realLink == nullptr ||
                       std::fabs(g_realLink->current.pos.x - lim.anchor[0]) > 300.0f ||
                       std::fabs(g_realLink->current.pos.y - lim.anchor[1]) > 300.0f ||
                       std::fabs(g_realLink->current.pos.z - lim.anchor[2]) > 300.0f;
    if (moved) {
        lim = CreateLimiter{};
        return false;
    }
    return true;
}

void RememberLimiterAnchor(CreateLimiter& lim) {
    lim.anchorRoom = LocalRoomNo();
    std::snprintf(lim.anchorStage, sizeof(lim.anchorStage), "%s", LocalStageName());
    if (g_realLink != nullptr) {
        lim.anchor[0] = g_realLink->current.pos.x;
        lim.anchor[1] = g_realLink->current.pos.y;
        lim.anchor[2] = g_realLink->current.pos.z;
        lim.hasAnchor = true;
    } else {
        lim.hasAnchor = false;
    }
}

/// True when we have a pose for `pid` on our current stage and room. Same-stage
/// other-room remotes must not materialize a full ALINK at local+120.
bool RemoteInLocalScene(PlayerId pid) {
    if (pid >= kMaxLocalPlayers || !g_receive[pid].hasState) {
        return false;
    }
    const char* local = LocalStageName();
    if (local == nullptr || local[0] == '\0') {
        return false;
    }
    if (std::strcmp(g_receive[pid].state.stage, local) != 0) {
        return false;
    }
    return g_receive[pid].state.roomNo == LocalRoomNo();
}

bool WantHorsePuppet(PlayerId pid) {
    if (pid >= kMaxLocalPlayers) {
        return false;
    }
    if (g_puppets[pid].state != SpawnState::Active) {
        return false;
    }
    const ReceiveSlot& slot = g_receive[pid];
    if (!slot.hasHorseState) {
        return false;
    }
    if ((slot.state.stateFlags & net::kPlayerStateFlagHorseRide) == 0) {
        return false;
    }
    return RemoteInLocalScene(pid);
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
        HorseEntry& h = g_horses[i];
        if (h.state == SpawnState::Creating || h.state == SpawnState::Active) {
            if (h.actor != nullptr) {
                fopAcM_delete(h.actor);
            } else if (h.pid != fpcM_ERROR_PROCESS_ID_e) {
                fopAcM_delete(h.pid);
            }
            h.state = SpawnState::Despawning;
        }
    }
    ClearCreatingAll();
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
        if ((st.stateFlags & net::kPlayerStateFlagHorseRide) == 0) {
            slot.hasHorseState = false;
        }
    } else if (type == net::MsgType::HorseState) {
        const auto& hs = payload.horseState;
        if (hs.playerId >= kMaxLocalPlayers || hs.playerId == SelfIdChecked()) {
            return;
        }
        ReceiveSlot& slot = g_receive[hs.playerId];
        slot.horse = hs;
        slot.hasHorseState = true;
    } else if (type == net::MsgType::PlayerEvent) {
        const auto& ev = payload.playerEvent;
        if (ev.playerId >= kMaxLocalPlayers || ev.playerId == SelfIdChecked()) {
            return;
        }
        ReceiveSlot& slot = g_receive[ev.playerId];
        const auto eventId = static_cast<net::PlayerEventId>(ev.eventId);
        if (eventId == net::PlayerEventId::Equip) {
            AppearanceState& ap = g_appearance[ev.playerId];
            ap.wantEquip = static_cast<u16>(ev.data & 0xFFFF);
            ap.wantSelectItem = static_cast<u8>((ev.data >> 16) & 0xFF);
            const u8 clothes = static_cast<u8>((ev.data >> 24) & 0xFF);
            ap.wantLeftJnt = static_cast<u16>(ev.data2 & 0xFFFF);
            ap.wantRightJnt = static_cast<u16>((ev.data2 >> 16) & 0xFFFF);
            ap.hasEquip = true;
            if (clothes != 0) {
                ap.wantClothes = clothes;
            }
            if (ev.scene != 0) {
                ap.wantSword = ev.scene;
            }
            if (ev.reserved != 0) {
                ap.wantShield = ev.reserved;
            }
            CoopLog.debug("coop: player {} equip {} sel {} clothes {} sword {} shield {} joints {}/{}",
                ev.playerId, ap.wantEquip, ap.wantSelectItem, clothes, ev.scene, ev.reserved,
                ap.wantLeftJnt, ap.wantRightJnt);
        } else if (eventId == net::PlayerEventId::SceneChange) {
            slot.state.roomNo = static_cast<s8>(ev.data & 0xFF);
            if (ev.scene == 1) {
                // Same-stage room change: keep the existing stage name.
            } else if (ev.stage[0] != '\0') {
                std::snprintf(slot.state.stage, sizeof(slot.state.stage), "%s", ev.stage);
            } else {
                // Pre-v10 peer: no destination name. Invalidate until PlayerState.
                slot.state.stage[0] = '\0';
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Sender
// ---------------------------------------------------------------------------

void SendPlayerEvent(net::PlayerEventId eventId, u32 data, u32 data2, u8 scene = 0,
                     u8 reserved = 0, const char* stage = nullptr) {
    net::PayloadUnion payload = {};
    payload.playerEvent.playerId = SelfIdChecked();
    payload.playerEvent.eventId = static_cast<u8>(eventId);
    payload.playerEvent.scene = scene;
    payload.playerEvent.reserved = reserved;
    payload.playerEvent.data = data;
    payload.playerEvent.data2 = data2;
    if (stage != nullptr && stage[0] != '\0') {
        std::snprintf(payload.playerEvent.stage, sizeof(payload.playerEvent.stage), "%s", stage);
    }
    if (!g_session.SendGameMessage(net::MsgType::PlayerEvent, payload)) {
        CoopLog.debug("coop: dropped PlayerEvent {} (session not sendable)", static_cast<u8>(eventId));
    }
}

void SendHorseState(const daAlink_c* link) {
    if (!link->checkHorseRide()) {
        return;
    }
    daHorse_c* horse = dComIfGp_getHorseActor();
    if (horse == nullptr || horse->m_model == nullptr || horse->m_model->getModelData() == nullptr) {
        return;
    }
    net::HorseStateMsg hs = {};
    hs.playerId = SelfIdChecked();
    hs.roomNo = fopAcM_GetRoomNo(horse);
    std::snprintf(hs.stage, sizeof(hs.stage), "%s", LocalStageName());
    const u16 jointCount = horse->m_model->getModelData()->getJointNum();
    hs.jointCount = jointCount < net::kMaxJoints ? static_cast<u8>(jointCount) : net::kMaxJoints;
    hs.yaw = horse->shape_angle.y;
    hs.pos.x = horse->current.pos.x;
    hs.pos.y = horse->current.pos.y;
    hs.pos.z = horse->current.pos.z;
    std::memcpy(hs.baseTR, horse->m_model->getBaseTRMtx(), sizeof(Mtx));
    for (u16 j = 0; j < hs.jointCount; ++j) {
        std::memcpy(hs.joints[j], horse->m_model->getAnmMtx(j), sizeof(Mtx));
        if (horse->m_model->getMtxBuffer()->getScaleFlag(j) != 0) {
            hs.scaleFlags[j >> 3] |= static_cast<u8>(1 << (j & 7));
        }
    }
    if (!PoseIsFinite(hs.pos, hs.baseTR, hs.joints, hs.jointCount)) {
        CoopLog.warn("coop: skipping non-finite local horse pose send");
        return;
    }
    net::PayloadUnion payload = {};
    payload.horseState = hs;
    g_session.SendGameMessage(net::MsgType::HorseState, payload);
}

void ResetSenderTrackers() {
    g_lastSentEquip = 0xFFFF;
    g_lastSentSelectItem = 0xFF;
    g_lastSentClothes = 0xFF;
    g_lastSentSword = 0xFF;
    g_lastSentShield = 0xFF;
    g_lastSentLeftJnt = 0xFFFF;
    g_lastSentRightJnt = 0xFFFF;
    g_lastSentRoom = 0x7F;
    g_lastSentStage[0] = '\0';
    g_postChangeSendWindow = 0;
    g_lastRosterMask = 0;
}

u8 PresentRemoteMask() {
    u8 mask = 0;
    const auto& roster = g_session.roster();
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        if (roster[i].present && i != SelfIdChecked()) {
            mask |= static_cast<u8>(1u << i);
        }
    }
    return mask;
}

/// Join/rejoin: Equip is change-detected. Reset the last-sent appearance so
/// SendEventsOnChange fires again the next time the sender gate is open.
void ResendAppearanceIfRosterGrew() {
    const u8 mask = PresentRemoteMask();
    const u8 gained = static_cast<u8>(mask & static_cast<u8>(~g_lastRosterMask));
    g_lastRosterMask = mask;
    if (gained == 0) {
        return;
    }
    g_lastSentEquip = 0xFFFF;
    g_lastSentSelectItem = 0xFF;
    g_lastSentClothes = 0xFF;
    g_lastSentSword = 0xFF;
    g_lastSentShield = 0xFF;
    g_lastSentLeftJnt = 0xFFFF;
    g_lastSentRightJnt = 0xFFFF;
}

void SendEventsOnChange(const daAlink_c* link) {
    // Form rides PlayerState.form every frame. AttentionChange was never
    // applied — queuing it dropped Equip. Only Equip is sent on change.
    const u16 equip = link->mEquipItem;
    const u8 selectItem = link->mSelectItemId;
    const u8 clothes = dComIfGs_getSelectEquipClothes();
    const u8 sword = dComIfGs_getSelectEquipSword();
    const u8 shield = dComIfGs_getSelectEquipShield();
    const u16 leftJnt = link->mLeftItemJntNo;
    const u16 rightJnt = link->mRightItemJntNo;
    if (equip != g_lastSentEquip || selectItem != g_lastSentSelectItem ||
        clothes != g_lastSentClothes || sword != g_lastSentSword || shield != g_lastSentShield ||
        leftJnt != g_lastSentLeftJnt || rightJnt != g_lastSentRightJnt)
    {
        g_lastSentEquip = equip;
        g_lastSentSelectItem = selectItem;
        g_lastSentClothes = clothes;
        g_lastSentSword = sword;
        g_lastSentShield = shield;
        g_lastSentLeftJnt = leftJnt;
        g_lastSentRightJnt = rightJnt;
        SendPlayerEvent(net::PlayerEventId::Equip,
            static_cast<u32>(equip) | (static_cast<u32>(selectItem) << 16) |
                (static_cast<u32>(clothes) << 24),
            static_cast<u32>(leftJnt) | (static_cast<u32>(rightJnt) << 16), sword, shield);
    }
    // The room-change event is NOT sent here: sendPlayerState() sends it
    // before the sender gate so a room change always propagates (MAJOR M2).
}

/// Sender gate (Anchor model): only send when at least one remote player is
/// in (or unknown to be outside) our scene — a client only renders peers in
/// its own stage+room, so same-scene peers are the only ones that can see us.
///
/// Capstone MINOR L (review-full-deepseek-v4-flash-0731.md MINOR L): this is
/// THE player sender gate. (The enemy snapshot sender that once shared it
/// was deleted with the authority stack.)
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

void ApplyStoredEquip(daAlink_c* link, PlayerId pid) {
    AppearanceState& ap = g_appearance[pid];
    if (!ap.hasEquip) {
        return;
    }
    link->mEquipItem = ap.wantEquip;
    link->mSelectItemId = ap.wantSelectItem;
    if (ap.wantLeftJnt != 0xFFFF) {
        link->mLeftItemJntNo = ap.wantLeftJnt;
    }
    if (ap.wantRightJnt != 0xFFFF) {
        link->mRightItemJntNo = ap.wantRightJnt;
    }
}

/// Restore this Link's per-instance anm-blend calc on the shared J3DModelData
/// joints. changeWolf/changeLink and puppet modelCalc overwrite those pointers;
/// the real Link's draw callbacks need its own calc objects installed.
void ReassertSharedMtxCalc(daAlink_c* link) {
    if (link == nullptr || link->mpLinkModel == nullptr) {
        return;
    }
    J3DModelData* md = link->mpLinkModel->getModelData();
    if (md == nullptr) {
        return;
    }
    if (link->checkWolf()) {
        md->getJointNodePointer(0)->setMtxCalc(link->field_0x1f20);
        md->getJointNodePointer(3)->setMtxCalc(link->field_0x1f24);
        md->getJointNodePointer(15)->setMtxCalc(link->field_0x1f20);
    } else {
        md->getJointNodePointer(0)->setMtxCalc(link->field_0x1f20);
        md->getJointNodePointer(1)->setMtxCalc(link->field_0x1f24);
        md->getJointNodePointer(16)->setMtxCalc(link->field_0x1f20);
    }
}

void ReassertRealLinkMtxCalc(const daAlink_c* puppet) {
    if (g_realLink != nullptr && g_realLink != puppet) {
        ReassertSharedMtxCalc(g_realLink);
    }
}

/// Shared J3DModelData hide/show (sword/sheath/clothes shapes, joint
/// callbacks) is mutated by puppet changeLink/setSelectEquipItem. Re-apply
/// the real Link's presentation so the local player does not inherit the
/// puppet's sheath/blade visibility.
void ReassertRealLinkPresentation(const daAlink_c* puppet) {
    if (g_realLink == nullptr || g_realLink == puppet) {
        return;
    }
    if (g_realLink->checkWolf()) {
        g_realLink->changeModelDataDirectWolf(0);
    } else {
        g_realLink->changeModelDataDirect(0);
    }
    g_realLink->setSelectEquipItem(FALSE);
    ReassertSharedMtxCalc(g_realLink);
}

bool FiniteF32(f32 v) {
    return std::isfinite(v);
}

bool PoseIsFinite(const net::Vec3f& pos, const Mtx& baseTR, const Mtx* joints, u8 jointCount) {
    if (!FiniteF32(pos.x) || !FiniteF32(pos.y) || !FiniteF32(pos.z)) {
        return false;
    }
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (!FiniteF32(baseTR[r][c])) {
                return false;
            }
        }
    }
    const u8 n = jointCount < net::kMaxJoints ? jointCount : net::kMaxJoints;
    for (u8 j = 0; j < n; ++j) {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                if (!FiniteF32(joints[j][r][c])) {
                    return false;
                }
            }
        }
    }
    return true;
}

/// Re-point anm packs after a body-arc reload (the old arc was freed).
static void RepointPuppetWaitAnm(daAlink_c* link, bool wantWolf) {
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
}

/// Drives the puppet's form / clothes / sword / shield to match the remote
/// player. Returns true once idle (skeleton and models match). While a body
/// or shield arc is loading the caller must hide the puppet (model data was
/// freed). Does not set mClothesChangeWaitTimer / mShieldChangeWaitTimer —
/// puppets skip execute(), and those timers would stall the destructor.
static bool DriveRemoteAppearance(daAlink_c* link, PlayerId pid, bool wantWolf) {
    AppearanceState& ap = g_appearance[pid];
    ap.wantWolf = wantWolf;

    const bool formMismatch = (link->checkWolf() != 0) != wantWolf;

    // First clothes Equip matching the local tunic: create already loaded
    // that arc via playerInit/changeLink — skip a redundant reload.
    if (!wantWolf && ap.wantClothes != 0 && ap.loadedClothes == 0 &&
        ap.wantClothes == dComIfGs_getSelectEquipClothes())
    {
        ap.loadedClothes = ap.wantClothes;
    }
    // First shield Equip matching the local shield: create already called
    // setShieldModel from the local save.
    if (ap.wantShield != 0 && ap.loadedShield == 0 &&
        ap.wantShield == dComIfGs_getSelectEquipShield())
    {
        ap.loadedShield = ap.wantShield;
    }

    const bool clothesMismatch =
        !wantWolf && ap.wantClothes != 0 && ap.loadedClothes != ap.wantClothes;
    const bool needBody = formMismatch || clothesMismatch || ap.arcLoadActive;

    if (needBody) {
        if (!ap.arcLoadActive) {
            ap.arcLoadActive = true;
            ap.loadWantWolf = wantWolf;
            dComIfG_resDelete(&link->mPhaseReq, link->mArcName);
            cPhs_Reset(&link->mPhaseReq);
            link->mpArcHeap->freeAll();
            {
                ScopedSelectEquip poke(ap.wantClothes, ap.wantSword, ap.wantShield);
                link->setArcName(ap.loadWantWolf ? TRUE : FALSE);
            }
        }
        if (dComIfG_resLoad(&link->mPhaseReq, link->mArcName, link->mpArcHeap) !=
            cPhs_COMPLEATE_e)
        {
            return false;
        }
        {
            ScopedSelectEquip poke(ap.wantClothes, ap.wantSword, ap.wantShield);
            if (ap.loadWantWolf) {
                link->changeWolf();
            } else {
                link->changeLink(0);
            }
            RepointPuppetWaitAnm(link, ap.loadWantWolf);
            link->setSelectEquipItem(FALSE);
            ReassertRealLinkPresentation(link);
        }
        ap.arcLoadActive = false;
        if (!ap.loadWantWolf && ap.wantClothes != 0) {
            ap.loadedClothes = ap.wantClothes;
        }
        if (ap.wantSword != 0) {
            ap.loadedSword = ap.wantSword;
        }
        // Form flipped mid-load — wait a frame so we start the other arc
        // from a complete skeleton instead of chaining two freeAlls.
        if ((link->checkWolf() != 0) != wantWolf) {
            return false;
        }
    }

    const bool shieldMismatch = ap.wantShield != 0 && ap.loadedShield != ap.wantShield;
    if (ap.shieldLoadActive || shieldMismatch) {
        if (!ap.shieldLoadActive) {
            ap.shieldLoadActive = true;
            link->mShieldModel = nullptr;
            dComIfG_resDelete(&link->mShieldPhaseReq, link->mShieldArcName);
            cPhs_Reset(&link->mShieldPhaseReq);
            link->mpShieldArcHeap->freeAll();
            {
                ScopedSelectEquip poke(ap.wantClothes, ap.wantSword, ap.wantShield);
                link->setShieldArcName();
            }
        }
        if (dComIfG_resLoad(&link->mShieldPhaseReq, link->mShieldArcName, link->mpShieldArcHeap) !=
            cPhs_COMPLEATE_e)
        {
            return false;
        }
        {
            ScopedSelectEquip poke(ap.wantClothes, ap.wantSword, ap.wantShield);
            link->setShieldModel();
        }
        ap.shieldLoadActive = false;
        ap.loadedShield = ap.wantShield;
    }

    // Sword models already live on the Alink arc. Always apply on first Equip
    // (create skipped setSelectEquipItem) even when the id matches the local
    // save — playerInit hardcodes the ordon sword model.
    if (ap.wantSword != 0 && ap.loadedSword != ap.wantSword) {
        ScopedSelectEquip poke(ap.wantClothes, ap.wantSword, ap.wantShield);
        link->setSelectEquipItem(FALSE);
        ap.loadedSword = ap.wantSword;
        ReassertRealLinkPresentation(link);
    }

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

    // Equip first so clothes/sword/shield are known before appearance drive.
    ApplyStoredEquip(link, pid);

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
    AppearanceState& ap = g_appearance[pid];
    if (entry.hidden) {
        // A body/shield reload already freed this puppet's model. Keep
        // driving the load even while room-hidden so we do not sit on a
        // dangling arc until they re-enter.
        if (ap.arcLoadActive || ap.shieldLoadActive) {
            DriveRemoteAppearance(link, pid, st.form != 0);
            ReassertRealLinkPresentation(link);
            entry.hidden = true;
        }
        return;
    }

    // Frozen-in-cutscene: while the remote player is inside a demo we hold
    // the last pose (accepted design — the puppet never runs demo code).
    if (st.stateFlags & net::kPlayerStateFlagDemo) {
        if (ap.arcLoadActive || ap.shieldLoadActive) {
            DriveRemoteAppearance(link, pid, st.form != 0);
            ReassertRealLinkPresentation(link);
        }
        return;
    }

    if (!PoseIsFinite(st.pos, st.baseTR, st.joints, st.jointCount)) {
        CoopLog.warn("coop: dropping non-finite pose from player {}", pid);
        return;
    }

    // 1) transform
    link->current.pos.x = st.pos.x;
    link->current.pos.y = st.pos.y;
    link->current.pos.z = st.pos.z;
    link->shape_angle.y = st.yaw;
    link->current.angle.y = st.yaw;
    link->mBodyAngle.x = st.pitch;

    // 2) form / clothes / sword / shield — target arcs load on demand;
    //    the host-save transform-status write inside changeWolf/changeLink
    //    is suppressed for puppets in d_a_alink_wolf.inc.
    const bool wantWolf = st.form != 0;
    if (!DriveRemoteAppearance(link, pid, wantWolf)) {
        entry.hidden = true;  // model data is freed mid-reload — do not draw
        ReassertRealLinkPresentation(link);
        return;
    }

    // 3) mProcID — PROC_WAIT v1 (pose replaces the action state machine)
    link->mProcID = daAlink_c::PROC_WAIT;

    // face expression on change (bck/btp indices; the face model calc inside
    // setItemMatrix/setWolfItemMatrix picks them up)
    // 0 is the vanilla "derive from lock-on" sentinel and would use the local
    // player's shared attention manager. Skip it; the pasted matrices already
    // hold the sender's face.
    if (st.faceBckIdx != 0 && st.faceBckIdx != 0xFFFF &&
        st.faceBckIdx != link->mFaceBckHeap.getIdx())
    {
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
    ReassertRealLinkPresentation(link);

    // 7) item/face/hat model attachment at the (synced) item joints.
    //    checkSwordGet / checkMasterSwordEquip read the SAVE select-equip;
    //    poke the remote's sword so sheath/sword presentation is theirs.
    {
        ScopedSelectEquip poke(ap.wantClothes, ap.wantSword, ap.wantShield);
        if (!link->checkWolf()) {
            link->setItemMatrix(0);
        } else {
            link->setWolfItemMatrix();
        }
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

    // 10) no Tg / Co / mass — puppets are visual only (parallel-worlds:
    //     a friend's body must not eat local hits or push local actors).
    //     dCcS clears leftover create-time cylinders every frame.
    // 11) do NOT CrrPos / setRoomInfo — Acch would shove the pasted pose
    //     onto local ground (fight swim/climb/Epona) and room comes from
    //     the wire.
    fopAcM_SetRoomNo(link, st.roomNo);
    ReassertRealLinkPresentation(link);
}

void ApplyHorseState(daHorse_c* horse) {
    const PlayerId pid = HorsePlayerId(fopAcM_GetID(horse));
    if (pid == kInvalidPlayerId) {
        return;
    }
    HorseEntry& entry = g_horses[pid];
    ReceiveSlot& slot = g_receive[pid];
    if (!slot.hasHorseState) {
        entry.hidden = true;
        return;
    }
    const net::HorseStateMsg& hs = slot.horse;
    const bool sameStage = std::strcmp(hs.stage, LocalStageName()) == 0;
    entry.hidden = !sameStage || hs.roomNo != LocalRoomNo() || g_puppets[pid].hidden;
    if (entry.hidden) {
        return;
    }
    if (horse->m_model == nullptr || horse->m_model->getModelData() == nullptr) {
        return;
    }
    if (!PoseIsFinite(hs.pos, hs.baseTR, hs.joints, hs.jointCount)) {
        CoopLog.warn("coop: dropping non-finite horse pose from player {}", pid);
        return;
    }

    horse->current.pos.x = hs.pos.x;
    horse->current.pos.y = hs.pos.y;
    horse->current.pos.z = hs.pos.z;
    horse->old.pos = horse->current.pos;
    horse->shape_angle.y = hs.yaw;
    horse->current.angle.y = hs.yaw;
    fopAcM_SetRoomNo(horse, hs.roomNo);

    const u16 localJoints = horse->m_model->getModelData()->getJointNum();
    const u16 copyCount = hs.jointCount < localJoints ? hs.jointCount : localJoints;
    for (u16 j = 0; j < copyCount; ++j) {
        Mtx mtx;
        std::memcpy(mtx, hs.joints[j], sizeof(Mtx));
        horse->m_model->setAnmMtx(j, mtx);
        horse->m_model->setScaleFlag(j, static_cast<u8>((hs.scaleFlags[j >> 3] >> (j & 7)) & 1));
    }
    Mtx baseMtx;
    std::memcpy(baseMtx, hs.baseTR, sizeof(Mtx));
    horse->m_model->setBaseTRMtx(baseMtx);
    fopAcM_SetMtx(horse, horse->m_model->getBaseTRMtx());
    horse->m_model->calcWeightEnvelopeMtx();
#ifdef TARGET_PC
    for (u16 i = 0; i < horse->m_model->getModelData()->getWEvlpMtxNum(); ++i) {
        dusk::frame_interp::record_final_mtx(horse->m_model->getWeightAnmMtx(i));
    }
#endif
    DisableHorseColliders(horse);
    horse->attention_info.flags = 0;
    ReassertRealHorseMtxCalc();
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
                ClearCreatingIf(static_cast<PlayerId>(i));
                e.retryFrame = g_frameCount + 30;
            } else if (g_frameCount - e.createStartFrame >= kCreateDeadlineFrames) {
                // MAJOR M3 + review H1: the create is alive but stuck in phase 2
                // on cPhs_INIT_e. Abort ONCE — move to Despawning so this
                // predicate cannot re-fire every subsequent frame (that used
                // to spam fopAcM_delete, explode deadlineHits past 3 in
                // ~50 ms, and permanently drop the spawn).
                CoopLog.warn(
                    "coop: puppet {} create stuck {} frames at cPhs_INIT (pid {}); aborting create",
                    i, g_frameCount - e.createStartFrame, e.pid);
                fopAcM_delete(e.pid);
                e.state = SpawnState::Despawning;
                e.actor = nullptr;
                CreateLimiter& lim = g_createLimiter[i];
                RememberLimiterAnchor(lim);
                if (++lim.deadlineHits >= kCreateDeadlineStrikes) {
                    lim.dropped = true;
                    CoopLog.warn(
                        "coop: puppet {} create hit its deadline {} times; dropping spawn",
                        i, lim.deadlineHits);
                }
            }
            // else: still multi-phase; onLinkCreated flips it to Active.
        } else if (e.state == SpawnState::Despawning) {
            if (e.pid != fpcM_ERROR_PROCESS_ID_e && fopAcM_SearchByID(e.pid) == nullptr) {
                ClearCreatingIf(static_cast<PlayerId>(i));
                e = PuppetEntry{};
            }
        } else if (e.state == SpawnState::Active) {
            if (fopAcM_SearchByID(e.pid) == nullptr) {
                // Stage changed / actor died. If the player is still in the
                // session, re-spawn; otherwise drop. Do not touch the create
                // lock — another pid may still be Creating.
                const bool stillPresent = g_session.roster()[i].present && i != SelfIdChecked();
                e = PuppetEntry{};
                if (stillPresent) {
                    e.state = SpawnState::Requested;
                    CoopLog.info("coop: player {} puppet died; re-requesting", i);
                }
            }
        }
    }

    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        HorseEntry& h = g_horses[i];
        if (h.state == SpawnState::Creating) {
            if (fopAcM_SearchByID(h.pid) == nullptr) {
                h.state = SpawnState::Requested;
                h.pid = fpcM_ERROR_PROCESS_ID_e;
                h.actor = nullptr;
                h.createStartFrame = 0;
                if (g_creatingHorse) {
                    ClearCreatingIf(static_cast<PlayerId>(i));
                }
                h.retryFrame = g_frameCount + 30;
            } else if (g_frameCount - h.createStartFrame >= kCreateDeadlineFrames) {
                CoopLog.warn(
                    "coop: horse puppet {} create stuck {} frames (pid {}); aborting",
                    i, g_frameCount - h.createStartFrame, h.pid);
                fopAcM_delete(h.pid);
                h.state = SpawnState::Despawning;
                h.actor = nullptr;
                CreateLimiter& lim = g_horseLimiter[i];
                RememberLimiterAnchor(lim);
                if (++lim.deadlineHits >= kCreateDeadlineStrikes) {
                    lim.dropped = true;
                    CoopLog.warn(
                        "coop: horse puppet {} create hit its deadline {} times; dropping spawn",
                        i, lim.deadlineHits);
                }
            }
        } else if (h.state == SpawnState::Despawning) {
            if (h.pid != fpcM_ERROR_PROCESS_ID_e && fopAcM_SearchByID(h.pid) == nullptr) {
                if (g_creatingHorse) {
                    ClearCreatingIf(static_cast<PlayerId>(i));
                }
                h = HorseEntry{};
            }
        } else if (h.state == SpawnState::Active) {
            if (fopAcM_SearchByID(h.pid) == nullptr) {
                const bool stillWant = WantHorsePuppet(static_cast<PlayerId>(i));
                h = HorseEntry{};
                if (stillWant) {
                    h.state = SpawnState::Requested;
                    CoopLog.info("coop: player {} horse puppet died; re-requesting", i);
                }
            }
        }
    }

    // Issue at most one create per frame (serialized — the vanilla create
    // path shares a static bgWaitFlg, and multi-phase creates must not
    // interleave; plan Rev 3 R4).
    if (CreateInFlight()) {
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
        if (LimiterStillDropped(g_createLimiter[i])) {
            continue;
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
        g_creatingPid = static_cast<PlayerId>(i);
        g_creatingHorse = false;
        CoopLog.info("coop: puppet spawn requested for player {} (pid {})", i, pid);
        break;  // serialized: one create in flight
    }

    if (CreateInFlight()) {
        return;
    }
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        HorseEntry& h = g_horses[i];
        if (h.state != SpawnState::Requested) {
            continue;
        }
        if (h.retryFrame > g_frameCount) {
            continue;
        }
        if (LimiterStillDropped(g_horseLimiter[i])) {
            continue;
        }
        if (!g_realLinkReady || g_realLink == nullptr) {
            continue;
        }
        if (!g_receive[i].hasHorseState) {
            continue;
        }
        const net::HorseStateMsg& hs = g_receive[i].horse;
        cXyz pos(hs.pos.x, hs.pos.y, hs.pos.z);
        csXyz angle(0, hs.yaw, 0);
        const int roomNo = hs.roomNo;

        base_process_class* realProc = reinterpret_cast<base_process_class*>(g_realLink);
        layer_class* savedLayer = fpcLy_CurrentLayer();
        if (realProc->layer_tag.layer == nullptr) {
            continue;
        }
        fpcLy_SetCurrentLayer(realProc->layer_tag.layer);
        const fpc_ProcID pid =
            fopAcM_create(fpcNm_HORSE_e, 0xFFFF, 0, &pos, roomNo, &angle, nullptr, -1, nullptr);
        fpcLy_SetCurrentLayer(savedLayer);

        if (pid == fpcM_ERROR_PROCESS_ID_e) {
            CoopLog.warn("coop: fopAcM_create(HORSE) failed for player {}", i);
            h.retryFrame = g_frameCount + 60;
            continue;
        }
        h.pid = pid;
        h.state = SpawnState::Creating;
        h.createStartFrame = g_frameCount;
        g_creatingPid = static_cast<PlayerId>(i);
        g_creatingHorse = true;
        CoopLog.info("coop: horse puppet spawn requested for player {} (pid {})", i, pid);
        break;
    }
}

void PumpSessionAndSpawns() {
    const bool live = SessionLive();
    if (!live) {
        if (g_wasLive) {
            ClearAllPuppets();
            g_receive = {};
            g_appearance = {};
            g_createLimiter = {};
            g_horseLimiter = {};
            ResetSenderTrackers();
            CoopLog.info("coop: session ended; puppets cleared");
        }
        g_wasLive = false;
        return;
    }
    g_wasLive = true;

    // Roster diff -> spawn requests / despawns. Stay-put: only materialize a
    // daAlink_c when the remote is on OUR stage and room. Off-scene remotes
    // keep their receive slot so we notice when they travel in.
    const auto& roster = g_session.roster();
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        const bool present = roster[i].present && i != SelfIdChecked();
        const bool wantPuppet = present && RemoteInLocalScene(i);
        PuppetEntry& e = g_puppets[i];
        if (wantPuppet) {
            if (e.state == SpawnState::None) {
                e.state = SpawnState::Requested;
            }
        } else {
            if (!present) {
                ClearRemoteSlot(i);
            }
            if (e.state == SpawnState::Creating || e.state == SpawnState::Active) {
                CoopLog.info("coop: player {} {}; despawning puppet", i,
                    present ? "left our room" : "left");
                if (e.actor != nullptr) {
                    fopAcM_delete(e.actor);
                } else if (e.pid != fpcM_ERROR_PROCESS_ID_e) {
                    fopAcM_delete(e.pid);
                }
                e.state = SpawnState::Despawning;
                e.actor = nullptr;
            } else if (e.state == SpawnState::Requested) {
                e = PuppetEntry{};
            }
        }
        HorseEntry& h = g_horses[i];
        if (WantHorsePuppet(static_cast<PlayerId>(i))) {
            if (h.state == SpawnState::None) {
                h.state = SpawnState::Requested;
            }
        } else {
            RequestHorseDespawn(h);
        }
    }
    PumpSpawns();
}

/// Capstone MINOR F: reset the failed-start state (and re-arm a restart after
/// a terminal client-side end) whenever a relevant net.* var changes. The
/// user edits these from the Settings -> Network tab / cvars to fix a
/// port-busy or join failure; without this the failed start retried the SAME
/// broken values forever. autoConnect is omitted: toggling "remember this
/// host" must not start or stop a session.
void RegisterNetVarCallbacks() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    const auto onNetVarChange = [](dusk::config::ConfigVarBase&, const void*) {
        g_startFailed = false;
        g_startFailNotified = false;
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
    dusk::config::subscribe(net::config::hostPort.getName(), onNetVarChange);
    dusk::config::subscribe(net::config::joinHost.getName(), onNetVarChange);
    dusk::config::subscribe(net::config::sessionName.getName(), onNetVarChange);
    dusk::config::subscribe(net::config::role.getName(), onNetVarChange);
}

void MigrateLegacyEnabled() {
    // config.json net.enabled was the old master switch. Client + on becomes
    // autoConnect; host + on does not auto-host. Launch `--cvar net.enabled`
    // (Override) is handled in ApplyLaunchAutostart, not here.
    if (net::config::enabled.getLayer() == dusk::config::ConfigVarLayer::Override) {
        return;
    }
    if (!net::config::enabled.getValue()) {
        return;
    }
    if (net::config::role.getValue() == "client") {
        net::config::autoConnect.setValue(true);
    }
    net::config::enabled.setValue(false);
    dusk::config::save();
    CoopLog.info("coop: migrated net.enabled to net.autoConnect / disconnected");
}

void ApplyLaunchAutostart() {
    // Host/Connect at prelaunch already set g_connected — keep that intent.
    if (g_connected) {
        return;
    }
    if (net::config::autoConnect.getValue()) {
        g_connected = true;
        g_forceClient = true;
        CoopLog.info("coop: autoConnect — will start a client session");
        return;
    }
    const bool legacyEnabled =
        net::config::enabled.getLayer() == dusk::config::ConfigVarLayer::Override &&
        net::config::enabled.getValue();
    if (legacyEnabled || net::config::connected.getValue()) {
        g_connected = true;
        CoopLog.info("coop: launch connected override — will start from net.role");
    }
}

void EnsureSession() {
    RegisterNetVarCallbacks();
    if (g_restartRequested) {
        g_restartRequested = false;
        if (g_sessionStarted) {
            g_session.Stop();
            g_sessionStarted = false;
            g_startFailed = false;
            g_rejectedNotified = false;
            CoopLog.info("coop: session restart requested; stopping this frame");
            return;  // start on the next frame after puppets see !live
        }
    }
    if (!g_sawFirstEnsure) {
        g_sawFirstEnsure = true;
        MigrateLegacyEnabled();
        ApplyLaunchAutostart();
    }
    if (!g_connected) {
        if (g_sessionStarted) {
            g_session.Stop();
            g_sessionStarted = false;
            g_startFailed = false;
            CoopLog.info("coop: disconnected; session stopped");
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
    cfg.stage.stage[0] = '\0';
    const bool isClient = g_forceClient || net::config::role.getValue() == "client";
    bool ok;
    if (isClient) {
        ok = g_session.StartClient(cfg);
    } else {
        ok = g_session.StartHost(cfg);
    }
    if (!ok) {
        g_startFailed = true;
        g_startFailFrame = g_frameCount;
        const char* reason = g_session.startFailureReason();
        if (reason == nullptr || reason[0] == '\0') {
            reason = "unknown cause";
        }
        if (!g_startFailNotified) {
            g_startFailNotified = true;
            CoopLog.error("coop: failed to start {} session ({}); retrying in 3s",
                isClient ? "client" : "host", reason);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s: %s",
                isClient ? "Could not start the client session" : "Could not host the session",
                reason);
            NotifyCoop("Co-op session failed to start", buf);
        }
        return;
    }
    g_sessionStarted = true;
    g_startFailNotified = false;
    g_session.SetGameMessageHandler(OnGameMessage);
    CoopLog.info("coop: {} session started (port {})", isClient ? "client" : "host",
        g_session.boundPort());
}

// ---------------------------------------------------------------------------
// M4 helpers — toasts, host-leave UX
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
        g_connected = false;
        CoopLog.warn("coop: host ended the session — returning to single-player");
        NotifyCoop("Host left", "The host ended the session. Remote players have gone home; "
                                 "your game continues as single-player.");
        break;
    case net::SessionEndReason::ConnectionLost:
        g_connected = false;
        CoopLog.warn("coop: connection to the host lost — returning to single-player");
        NotifyCoop("Host disconnected", "Lost the connection to the host. Remote players have "
                                         "gone home; your game continues as single-player.");
        break;
    default:
        break;  // kicked / shutdown: no toast (or covered elsewhere)
    }
}

void ArmSession() {
    g_restartRequested = false;
    g_startFailed = false;
    g_startFailNotified = false;
    g_rejectedNotified = false;
    g_forceClient = false;
    g_connected = true;
    if (g_sessionStarted) {
        g_restartRequested = true;
        CoopLog.info("coop: Host/Connect — restart armed");
    } else {
        CoopLog.info("coop: Host/Connect — will start from current net.* CVars");
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// sessionActive() is used by enemies and the Network tab.
// hostRole() is used by the same. selfId()/remoteCount() ARE used (sender +
// modelCalc gate).

bool sessionActive() {
    return SessionLive();
}

bool sessionStarted() {
    return g_sessionStarted;
}

bool connected() {
    return g_connected;
}

void requestHost() {
    net::config::role.setValue("host");
    dusk::config::save();
    ArmSession();
}

void requestConnect() {
    net::config::role.setValue("client");
    dusk::config::save();
    ArmSession();
}

void requestDisconnect() {
    g_restartRequested = false;
    g_startFailed = false;
    g_startFailNotified = false;
    g_forceClient = false;
    g_connected = false;
    CoopLog.info("coop: Disconnect");
}

bool sessionWouldRestart() {
    if (!g_sessionStarted) {
        return false;
    }
    switch (g_session.state()) {
    case net::SessionState::Listening:
    case net::SessionState::Connecting:
    case net::SessionState::Connected:
    case net::SessionState::Joined:
        return true;
    default:
        return false;
    }
}

bool hostingCurrentSettings() {
    if (!hostRole()) {
        return false;
    }
    const auto state = g_session.state();
    if (state != net::SessionState::Listening && state != net::SessionState::Joined) {
        return false;
    }
    if (g_session.boundPort() != net::config::hostPort.getValue()) {
        return false;
    }
    return g_session.configName() == net::config::sessionName.getValue();
}

bool connectingCurrentSettings() {
    if (!g_sessionStarted || g_session.role() != net::SessionRole::Client) {
        return false;
    }
    switch (g_session.state()) {
    case net::SessionState::Connecting:
    case net::SessionState::Connected:
    case net::SessionState::Joined:
        break;
    default:
        return false;
    }
    if (g_session.configPort() != net::config::hostPort.getValue()) {
        return false;
    }
    if (g_session.configName() != net::config::sessionName.getValue()) {
        return false;
    }
    return g_session.configJoinHost() == TrimCopy(net::config::joinHost.getValue());
}

const char* joinTargetError() {
    const std::string host = TrimCopy(net::config::joinHost.getValue());
    if (host.empty()) {
        return "Join Host IP is empty. Enter the host's LAN IPv4 or hostname.";
    }
    if (host.find(':') != std::string::npos) {
        return "Join Host IP cannot contain a colon. Put the port in Port. IPv6 is not supported.";
    }
    return nullptr;
}

bool hostPortValid() {
    return net::config::hostPort.getValue() != 0;
}

const char* sessionStatusLabel() {
    static char buf[512];
    auto remainSuffix = [](char* out, size_t cap, size_t n) {
        if (n >= cap) {
            return;
        }
        const u32 sec = DeadlineRemainSec();
        if (sec > 0) {
            std::snprintf(out + n, cap - n, " (%us)", sec);
        }
    };
    if (!dusk::IsGameLaunched) {
        const bool asClient =
            g_forceClient || net::config::role.getValue() == "client";
        if (g_connected) {
            if (asClient) {
                std::snprintf(buf, sizeof(buf), "Will connect to %s:%u when the game launches",
                    net::config::joinHost.getValue().c_str(),
                    static_cast<unsigned>(net::config::hostPort.getValue()));
                return buf;
            }
            const char* ips = net::localIpv4Label();
            if (ips != nullptr && ips[0] != '\0') {
                std::snprintf(buf, sizeof(buf), "Will host on %s:%u when the game launches", ips,
                    static_cast<unsigned>(net::config::hostPort.getValue()));
            } else {
                std::snprintf(buf, sizeof(buf), "Will host on port %u when the game launches",
                    static_cast<unsigned>(net::config::hostPort.getValue()));
            }
            return buf;
        }
        if (net::config::autoConnect.getValue()) {
            std::snprintf(buf, sizeof(buf), "Will connect to %s:%u when the game launches",
                net::config::joinHost.getValue().c_str(),
                static_cast<unsigned>(net::config::hostPort.getValue()));
            return buf;
        }
        return "Disconnected";
    }
    if (g_restartRequested) {
        return "Restarting...";
    }
    if (!g_connected && !g_sessionStarted) {
        return "Disconnected";
    }
    if (g_startFailed) {
        const char* reason = g_session.startFailureReason();
        if (reason == nullptr || reason[0] == '\0') {
            reason = "unknown cause";
        }
        const u32 elapsed = g_frameCount - g_startFailFrame;
        const u32 remainFrames = elapsed >= 180 ? 0 : 180 - elapsed;
        const u32 remainSec = (remainFrames + 59) / 60;
        std::snprintf(buf, sizeof(buf), "Start failed (%s); retrying in %us", reason,
            remainSec == 0 ? 1 : remainSec);
        return buf;
    }
    if (!g_sessionStarted) {
        if (!g_connected) {
            return "Disconnected";
        }
        return "Starting...";
    }
    switch (g_session.state()) {
    case net::SessionState::Listening: {
        size_t n = 0;
        const char* ips = net::localIpv4Label();
        const unsigned port = static_cast<unsigned>(g_session.boundPort());
        const int players = remoteCount() + 1;
        if (ips != nullptr && ips[0] != '\0') {
            n = static_cast<size_t>(std::snprintf(buf, sizeof(buf), "Hosting %s:%u (%d/%d: ", ips,
                port, players, static_cast<int>(kMaxLocalPlayers)));
        } else {
            n = static_cast<size_t>(std::snprintf(buf, sizeof(buf), "Hosting on port %u (%d/%d: ",
                port, players, static_cast<int>(kMaxLocalPlayers)));
        }
        if (n >= sizeof(buf)) {
            n = sizeof(buf) - 1;
        }
        AppendRosterNames(buf, sizeof(buf), n);
        if (n + 1 < sizeof(buf)) {
            buf[n++] = ')';
            buf[n] = '\0';
        }
        return buf;
    }
    case net::SessionState::Connecting: {
        size_t n = static_cast<size_t>(std::snprintf(buf, sizeof(buf), "Connecting to %s:%u",
            net::config::joinHost.getValue().c_str(),
            static_cast<unsigned>(net::config::hostPort.getValue())));
        remainSuffix(buf, sizeof(buf), n);
        return buf;
    }
    case net::SessionState::Connected: {
        size_t n = static_cast<size_t>(std::snprintf(buf, sizeof(buf), "Connected; joining..."));
        remainSuffix(buf, sizeof(buf), n);
        return buf;
    }
    case net::SessionState::Joined: {
        size_t n = static_cast<size_t>(std::snprintf(buf, sizeof(buf), "Connected (%d/%d: ",
            remoteCount() + 1, static_cast<int>(kMaxLocalPlayers)));
        AppendRosterNames(buf, sizeof(buf), n);
        if (n + 1 < sizeof(buf)) {
            buf[n++] = ')';
            buf[n] = '\0';
        }
        return buf;
    }
    case net::SessionState::Rejected:
        std::snprintf(buf, sizeof(buf), "Join rejected (%s)", g_session.rejectReasonName());
        return buf;
    case net::SessionState::Ended:
        return "Disconnected";
    case net::SessionState::Idle:
    default:
        return "Idle";
    }
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

void onLinkCreated(daAlink_c* link) {
    if (isPuppet(link)) {
        const PlayerId pid = puppetPlayerId(fopAcM_GetID(link));
        if (pid != kInvalidPlayerId) {
            PuppetEntry& e = g_puppets[pid];
            e.actor = link;
            e.state = SpawnState::Active;
            e.hidden = true;  // until the first received state marks the room
            link->attention_info.flags = 0;
            if (g_creatingPid == pid) {
                g_creatingPid = kInvalidPlayerId;
                g_creatingHorse = false;
            }
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
            if (!g_creatingHorse) {
                ClearCreatingIf(playerId);
            }
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
    ResendAppearanceIfRosterGrew();
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
    // current stage (scene=1) or a stage change (scene=0). v7: the host no
    // longer sniffs rooms (ownership table gone); the receive-side puppet
    // gate adopts the event's room for same-stage moves, and the destination
    // stage name (v10) so a dropped unreliable burst cannot blank the remote.
    const bool stageChanged = std::strcmp(myStage, g_lastSentStage) != 0;
    if (roomNow != g_lastSentRoom || stageChanged) {
        g_lastSentRoom = roomNow;
        std::snprintf(g_lastSentStage, sizeof(g_lastSentStage), "%s", myStage);
        g_postChangeSendWindow = 30;
        SendPlayerEvent(net::PlayerEventId::SceneChange,
            static_cast<u32>(static_cast<s32>(roomNow)), 0,
            /*sameStage=*/stageChanged ? 0u : 1u, 0, myStage);
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
    if (link->checkHorseRide()) {
        st.stateFlags |= net::kPlayerStateFlagHorseRide;
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
    if (!PoseIsFinite(st.pos, st.baseTR, st.joints, static_cast<u8>(st.jointCount))) {
        CoopLog.warn("coop: skipping non-finite local pose send");
    } else {
        g_session.SendGameMessage(net::MsgType::PlayerState, payload);
    }

    SendHorseState(link);

    SendEventsOnChange(link);
}

bool puppetDrawHidden(const daAlink_c* link) {
    const PlayerId pid = puppetPlayerId(fopAcM_GetID(link));
    if (pid == kInvalidPlayerId) {
        return false;
    }
    return g_puppets[pid].hidden;
}

bool isHorsePuppet(const daHorse_c* horse) {
    if (horse == nullptr) {
        return false;
    }
    return EntryForHorsePid(fopAcM_GetID(horse)) != nullptr;
}

int horsePuppetExecute(daHorse_c* horse) {
    ApplyHorseState(horse);
    return 1;
}

bool horsePuppetDrawHidden(const daHorse_c* horse) {
    const PlayerId pid = HorsePlayerId(fopAcM_GetID(horse));
    if (pid == kInvalidPlayerId) {
        return false;
    }
    return g_horses[pid].hidden;
}

void onHorseCreated(daHorse_c* horse) {
    const PlayerId pid = HorsePlayerId(fopAcM_GetID(horse));
    if (pid == kInvalidPlayerId) {
        return;
    }
    HorseEntry& e = g_horses[pid];
    e.actor = horse;
    e.state = SpawnState::Active;
    e.hidden = true;
    horse->attention_info.flags = 0;
    DisableHorseColliders(horse);
    if (g_creatingHorse && g_creatingPid == pid) {
        g_creatingPid = kInvalidPlayerId;
        g_creatingHorse = false;
    }
    ReassertRealHorseMtxCalc();
    g_horseLimiter[pid] = CreateLimiter{};
    CoopLog.info("coop: horse puppet for player {} active (pid {})", pid, e.pid);
}

void onHorseDestroyed(daHorse_c* horse) {
    const fpc_ProcID pid = fopAcM_GetID(horse);
    const PlayerId playerId = HorsePlayerId(pid);
    if (playerId == kInvalidPlayerId) {
        return;
    }
    CoopLog.info("coop: horse puppet for player {} destroyed (pid {})", playerId, pid);
    g_horses[playerId] = HorseEntry{};
    if (g_creatingHorse) {
        ClearCreatingIf(playerId);
    }
}

void onGameFrame() {
    ++g_frameCount;
    // Retry a failed start after a 3 s backoff. This MUST run even when
    // g_sessionStarted is false (a failed start never sets that flag).
    if (g_startFailed && g_frameCount - g_startFailFrame >= 180) {
        g_startFailed = false;
    }
    EnsureSession();
    // Capstone MINOR F: the client's join rejection gets a ONE-SHOT toast
    // (version mismatch / session full / invalid slot / join timeout) —
        // previously only a log line, leaving the client stuck until the user
        // pressed Host/Connect again (which now also resets via a net.* var
        // change).
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
        g_session.Update();
        // Capstone MAJOR 1 (glue half — the review's "alternatively"): a
        // session that ended from the host's side (SessionEnd / connection
        // loss) reaches Ended with the transport STILL running. Stop() now
        // always tears the transport down; call it the frame we observe Ended
        // so the socket thread is released promptly. g_sessionStarted stays
        // set, so the session is NOT auto-restarted here — the user re-arms a
        // new session via Host/Connect (or a net.* var change, which resets
        // the failed-start state, MINOR F).
        if (g_session.state() == net::SessionState::Ended && g_session.transportRunning()) {
            g_session.Stop();
            CoopLog.info("coop: session ended remotely; transport torn down");
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
    // both players share a stage). setLocalRoom is gone with the
    // ownership table — the host's own room feeds nothing here anymore.
    if (g_sessionStarted && hostRole() && g_realLinkReady && g_realLink != nullptr) {
        net::StageInfo st = {};
        std::snprintf(st.stage, sizeof(st.stage), "%s", LocalStageName());
        st.room = fopAcM_GetRoomNo(g_realLink);
        st.layer = dComIfGp_getStartStageLayer();
        st.point = dComIfGp_getStartStagePoint();
        g_session.setWorldStage(st);
    }
    // M4: host-leave UX. Then drop the sticky Ended/Connecting-fail leftover so
    // the Network tab does not sit on "Session ended" with Disconnect still
    // enabled. A pre-join loss (Connecting/Connected) is a start failure and
    // retries like DNS/port-busy; a live session end stays disconnected.
    NoticeSessionEnd(wasLive && !SessionLive());
    if (g_sessionStarted && g_session.state() == net::SessionState::Ended) {
        if (wasLive) {
            g_sessionStarted = false;
        } else if (g_connected) {
            g_sessionStarted = false;
            g_startFailed = true;
            g_startFailFrame = g_frameCount;
            const char* reason = g_session.startFailureReason();
            if (reason == nullptr || reason[0] == '\0') {
                reason = "could not reach the host";
            }
            if (!g_startFailNotified) {
                g_startFailNotified = true;
                CoopLog.error("coop: connect failed ({}); retrying in 3s", reason);
                NotifyCoop("Could not connect", reason);
            }
        } else {
            g_sessionStarted = false;
        }
    }
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

}  // namespace dusk::coop
