#include "dusk/coop/coop_enemy.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_combat.h"
#include "dusk/coop/coop_context.h"
#include "dusk/coop/coop_entity_logic.h"

#include "d/actor/d_a_b_tn.h"
#include "d/actor/d_a_e_ai.h"
#include "d/actor/d_a_e_df.h"
#include "d/actor/d_a_e_hm.h"
#include "d/actor/d_a_e_md.h"
#include "d/actor/d_a_e_yc.h"
#include "d/d_com_inf_game.h"
#include "d/d_save.h"
#include "f_op/f_op_actor_iter.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_layer.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_mtx.h"

#include <aurora/lib/logging.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

namespace dusk::coop::enemy {

namespace {

aurora::Module EnemyLog("dusk::coop::enemy");

using net::kInvalidPlayerId;

constexpr u8 kSnapshotFlagDead = 1 << 0;  // host-side isDead window (death anim)

// ---------------------------------------------------------------------------
// Per-type callbacks — everything below goes through PUBLIC members of the
// game classes (03-enemies.md §7; m2-design-notes.md §2).
// ---------------------------------------------------------------------------

// --- E_AI (Armos): hit-count semantics, health meaningless (reset to 1000) ---

// m_modelMorf is private in e_ai_class (d_a_e_ai.h: 0x5D0).
void aiDriveModel(fopAc_ac_c* actor, const net::EnemySnapshotMsg& snap) {
    static_cast<e_ai_class*>(actor)->setBaseMtx();
    auto* morf = *reinterpret_cast<mDoExt_McaMorfSO**>(reinterpret_cast<u8*>(actor) + 0x5D0);
    if (morf != nullptr) {
        morf->setFrame(static_cast<f32>(snap.anim & 0xFFFF));
    }
}

void aiRefreshColliders(fopAc_ac_c* actor) {
    static_cast<e_ai_class*>(actor)->setCcCylinder();
}

bool aiIsDead(const fopAc_ac_c*) {
    // m_hitCount is private; Armos death is detected via actor deletion (the
    // universal backstop). health is meaningless (always reset to 1000).
    return false;
}

u8 aiDeathSwitch(const fopAc_ac_c* actor) {
    return static_cast<u8>((fopAcM_GetParam(actor) >> 16) & 0xFF);
}

// --- E_HM (Torch Slug): inline At_Check, health is real ---

// mAnm_p is private in daE_HM_c (d_a_e_hm.h: 0x618).
void hmDriveModel(fopAc_ac_c* actor, const net::EnemySnapshotMsg& snap) {
    static_cast<daE_HM_c*>(actor)->setBaseMtx();
    auto* morf = *reinterpret_cast<mDoExt_McaMorfSO**>(reinterpret_cast<u8*>(actor) + 0x618);
    if (morf != nullptr) {
        morf->setFrame(static_cast<f32>(snap.anim & 0xFFFF));
    }
}

void hmRefreshColliders(fopAc_ac_c* actor) {
    static_cast<daE_HM_c*>(actor)->setCcCylinder();
}

bool hmIsDead(const fopAc_ac_c* actor) {
    // At_Check decrements health on every hit; the state machine then decides
    // (ground slugs die on the first hit). health <= 0 marks the death window.
    return actor->health <= 0;
}

u8 hmDeathSwitch(const fopAc_ac_c* actor) {
    return static_cast<u8>((fopAcM_GetParam(actor) >> 24) & 0xFF);
}

// --- E_DF (Deku Flower): reaction-only (bounds on hit; never dies from damage) ---

void dfDriveModel(fopAc_ac_c* actor, const net::EnemySnapshotMsg& snap) {
    daE_DF_c* df = static_cast<daE_DF_c*>(actor);
    df->setBaseMtx();
    if (df->mpMorfSO != nullptr) {
        df->mpMorfSO->setFrame(static_cast<f32>(snap.anim & 0xFFFF));
    }
}

void dfRefreshColliders(fopAc_ac_c* actor) {
    static_cast<daE_DF_c*>(actor)->setCcCylinder();
}

bool dfIsDead(const fopAc_ac_c*) {
    return false;  // death only via the eat sequence; actor-deletion backstop
}

u8 dfDeathSwitch(const fopAc_ac_c* actor) {
    return static_cast<u8>(fopAcM_GetParam(actor) & 0xFF);
}

// --- E_YC (Twilight Kargorok): flying, wolf-bite only; no public mtx member ---

void ycDriveModel(fopAc_ac_c* actor, const net::EnemySnapshotMsg& snap) {
    e_yc_class* yc = static_cast<e_yc_class*>(actor);
    if (yc->mpMorf == nullptr) {
        return;
    }
    J3DModel* model = yc->mpMorf->getModel();
    // Preserve the model's own per-instance scale (l_HIO.mScale is a
    // file-static in the actor TU, not reachable here): extract column
    // magnitudes from the current baseTRMtx, then rebuild
    // transS(pos) * rotY * rotX * rotZ * scale.
    Mtx cur;
    std::memcpy(cur, model->getBaseTRMtx(), sizeof(Mtx));
    auto colMag = [&cur](u8 c) {
        return std::sqrtf(cur[0][c] * cur[0][c] + cur[1][c] * cur[1][c] +
                          cur[2][c] * cur[2][c]);
    };
    const f32 sx = colMag(0);
    const f32 sy = colMag(1);
    const f32 sz = colMag(2);
    mDoMtx_stack_c::transS(actor->current.pos.x, actor->current.pos.y, actor->current.pos.z);
    mDoMtx_stack_c::YrotM(actor->shape_angle.y);
    mDoMtx_stack_c::XrotM(actor->shape_angle.x);
    mDoMtx_stack_c::ZrotM(actor->shape_angle.z);
    mDoMtx_stack_c::scaleM(sx > 0.0f ? sx : 1.0f, sy > 0.0f ? sy : 1.0f, sz > 0.0f ? sz : 1.0f);
    model->setBaseTRMtx(mDoMtx_stack_c::get());
    yc->mpMorf->modelCalc();
    yc->mpMorf->setFrame(static_cast<f32>(snap.anim & 0xFFFF));
    // eyePos from bone 1 (draw + lock-on read it).
    Mtx boneMtx;
    std::memcpy(boneMtx, model->getAnmMtx(1), sizeof(Mtx));
    cXyz zero(0.0f, 0.0f, 0.0f);
    cXyz eye;
    cMtx_multVec(boneMtx, &zero, &eye);
    actor->eyePos = eye;
    actor->attention_info.position = eye;
    actor->attention_info.position.y += 50.0f;
}

void ycRefreshColliders(fopAc_ac_c* actor) {
    e_yc_class* yc = static_cast<e_yc_class*>(actor);
    if (yc->mpMorf == nullptr) {
        return;
    }
    Mtx boneMtx;
    std::memcpy(boneMtx, yc->mpMorf->getModel()->getAnmMtx(1), sizeof(Mtx));
    cXyz zero(0.0f, 0.0f, 0.0f);
    cXyz center;
    cMtx_multVec(boneMtx, &zero, &center);
    yc->mCcSph.SetC(center);
    dComIfG_Ccsp()->Set(&yc->mCcSph);
}

bool ycIsDead(const fopAc_ac_c* actor) {
    // health is real but only decremented by wolf bites; death anim window.
    return actor->health <= 0;
}

u8 ycDeathSwitch(const fopAc_ac_c*) {
    return 0xFF;
}

// --- E_MD (Suit of Armor): breaks via iron-ball/hammer, health never used ---

void mdDriveModel(fopAc_ac_c* actor, const net::EnemySnapshotMsg& snap) {
    daE_MD_c* md = static_cast<daE_MD_c*>(actor);
    md->setBaseMtx();
    if (md->mpModelMorf != nullptr) {
        md->mpModelMorf->setFrame(static_cast<f32>(snap.anim & 0xFFFF));
    }
}

void mdRefreshColliders(fopAc_ac_c* actor) {
    // setCcCylinder(height) re-registers mCyl at current.pos; preserve the
    // instance's own height/radius (vanilla passes 350/100 by type).
    daE_MD_c* md = static_cast<daE_MD_c*>(actor);
    md->setCcCylinder(md->mCyl.GetH() > 0.0f ? md->mCyl.GetH() : 350.0f);
}

bool mdIsDead(const fopAc_ac_c*) {
    return false;  // break path; actor-deletion backstop
}

u8 mdDeathSwitch(const fopAc_ac_c* actor) {
    return static_cast<u8>((fopAcM_GetParam(actor) >> 8) & 0xFF);
}

// --- B_TN (Darknut, ToT mini-boss): shared cc_at_check, health 100 ---

// mpModelMorf2 is private in daB_TN_c (d_a_b_tn.h: 0x600).
void tnDriveModel(fopAc_ac_c* actor, const net::EnemySnapshotMsg& snap) {
    static_cast<daB_TN_c*>(actor)->mtx_set();
    auto* morf = *reinterpret_cast<mDoExt_McaMorfSO**>(reinterpret_cast<u8*>(actor) + 0x600);
    if (morf != nullptr) {
        morf->setFrame(static_cast<f32>(snap.anim & 0xFFFF));
    }
}

void tnRefreshColliders(fopAc_ac_c* actor) {
    static_cast<daB_TN_c*>(actor)->cc_set();
}

bool tnIsDead(const fopAc_ac_c* actor) {
    return actor->health <= 0;
}

u8 tnDeathSwitch(const fopAc_ac_c* actor) {
    return static_cast<u8>(fopAcM_GetParam(actor) & 0xFF);
}

// ---------------------------------------------------------------------------
// The whitelist (m2-design-notes.md §2, revised by per-type verification):
//   E_AI  Armos             hit-count death, drops 0x1E
//   E_HM  Torch Slug        health + state-machine death, drops 0x23
//   E_DF  Deku Flower       reaction-only (bounds), dies via eat sequence
//   E_YC  Twilight Kargorok wolf-bite damage only, flying
//   E_MD  Suit of Armor     breaks via iron-ball/hammer
//   B_TN  Darknut (boss)    shared cc_at_check, per-player switch grant
// Dropped vs the earlier draft: E_GS (no damage collider — m2-design-notes
// §2) and E_FB (Freezard — iron-ball-only damage, its handler casts
// GetTgHitAc() to daObjCarry_c and reads slot 0: the injection path cannot
// drive it safely).
// ---------------------------------------------------------------------------

// Forward declarations for the per-type injectHit callbacks.
void aiInjectHit(fopAc_ac_c*, const net::CombatIntentMsg&);
void hmInjectHit(fopAc_ac_c*, const net::CombatIntentMsg&);
void dfInjectHit(fopAc_ac_c*, const net::CombatIntentMsg&);
void ycInjectHit(fopAc_ac_c*, const net::CombatIntentMsg&);
void mdInjectHit(fopAc_ac_c*, const net::CombatIntentMsg&);
void tnInjectHit(fopAc_ac_c*, const net::CombatIntentMsg&);

const std::array<NetEnemyAdapter, 6> kAdapters{{
    {fpcNm_E_AI_e, "E_AI", DamageSemantics::HitCount, 0x1E, 0, false, aiRefreshColliders,
        aiDriveModel, aiInjectHit, aiIsDead, aiDeathSwitch},
    {fpcNm_E_HM_e, "E_HM", DamageSemantics::Hp, 0x23, 1, false, hmRefreshColliders,
        hmDriveModel, hmInjectHit, hmIsDead, hmDeathSwitch},
    {fpcNm_E_DF_e, "E_DF", DamageSemantics::Special, 0x30, 1, false, dfRefreshColliders,
        dfDriveModel, dfInjectHit, dfIsDead, dfDeathSwitch},
    {fpcNm_E_YC_e, "E_YC", DamageSemantics::Hp, 0xFF, 0, false, ycRefreshColliders,
        ycDriveModel, ycInjectHit, ycIsDead, ycDeathSwitch},
    {fpcNm_E_MD_e, "E_MD", DamageSemantics::Special, 0xFF, 0, false, mdRefreshColliders,
        mdDriveModel, mdInjectHit, mdIsDead, mdDeathSwitch},
    {fpcNm_B_TN_e, "B_TN", DamageSemantics::Hp, 29, 1, true, tnRefreshColliders,
        tnDriveModel, tnInjectHit, tnIsDead, tnDeathSwitch},
}};

// ---------------------------------------------------------------------------
// Registry — enemyId <-> actor, per stage.
// ---------------------------------------------------------------------------

enum class EntryState : u8 {
    Registered,  // normal (host: simmed + snapshotted; client: frozen puppet)
    Dying,       // client: died event received; drop+VFX spawned; delete pending
};

struct EnemyEntry {
    u16 enemyId = kInvalidEnemyId;
    fpc_ProcID pid = fpcM_ERROR_PROCESS_ID_e;
    fopAc_ac_c* actor = nullptr;
    s8 roomNo = -1;
    // Capstone MAJOR 2 (review-full-glm-5.2.md MAJOR 1): the per-type
    // adapter is captured at REGISTRATION and held on the entry. kAdapters is
    // a static std::array that outlives every entry, so the pointer stays
    // valid for the entry's whole lifetime — PollHostDeaths must never call
    // AdapterForActor(e.actor) AFTER the actor is freed (the death poll can
    // run 2+ frames after the host's delete queue freed the actor: the poll
    // runs pre-fpcM_Management, and fpcDt_Handler frees the actor at the top
    // of the next frame's management pass).
    const NetEnemyAdapter* adapter = nullptr;
    // Owner-side synthetic attacker collider for this enemy (one per entry:
    // the enemy's handler reads GetTgHitObj() during its own execute, so a
    // shared static would let a same-frame second injection corrupt the first
    // enemy's hit data). SetTgHitSynthetic stores &synthAt into the enemy's
    // damage collider and &synthStts into the synth — those ADDRESSES must
    // outlive the injection. g_entries is a NODE-BASED std::map so entry
    // addresses are stable across inserts (capstone MINOR H: an
    // unordered_map rehash would relocate live entries and dangle them).
    // Same-frame consumption contract: inject (pre-actor) -> the enemy's own
    // execute consumes the Tg flag and clears it -> the draw-phase collision
    // pass sees no flag. An execute-skip (suspend box / freeze) between
    // injection and the collision pass leaves the flag set for one frame —
    // benign as long as the addresses remain valid (which the node-based
    // container guarantees).
    dCcD_Sph synthAt;
    dCcD_Stts synthStts;
    // Per-entry cullMtx (M2.5, MAJOR-2 fix): fopAcM_SetMtx stores a POINTER,
    // so a shared static/thread_local buffer would make every frozen puppet
    // cull against the LAST puppet's matrix. A stable per-entry address keeps
    // each puppet's frustum check on its own position.
    Mtx cullMtx{};
    EnemyEntry() = default;
    EnemyEntry(const EnemyEntry&) = delete;
    EnemyEntry& operator=(const EnemyEntry&) = delete;
    EnemyEntry(EnemyEntry&&) = default;
    EnemyEntry& operator=(EnemyEntry&&) = default;
    EntryState state = EntryState::Registered;
    u32 dyingFrame = 0;  // frame when the client's death beat started
    u8 deathSwitch = 0xFF;
    bool diedSent = false;
    // Host sender dirty-tracking.
    f32 lastPos[3] = {0.0f, 0.0f, 0.0f};
    f32 lastSpeed[3] = {0.0f, 0.0f, 0.0f};
    s16 lastAngle = 0;
    u16 lastHp = 0;
    u8 lastFlags = 0xFF;  // 0xFF forces the first send
    u32 lastAnim = 0;
    // Frame-batched combat result latch: set by applyInjectedHit, consumed
    // by hostOnExecuted AFTER the enemy's own execute ran the injected hit
    // (so the CombatResult reports the true post-hit HP).
    bool pendingHit = false;
    u16 hitDamage = 0;
    u32 hitSeq = 0;
    u8 hitAttacker = kInvalidPlayerId;
};

// Capstone MINOR H (review-full-deepseek-v4-flash-0731.md MINOR H):
// g_entries is a NODE-BASED container (std::map) so EnemyEntry addresses are
// stable across inserts — SetTgHitSynthetic stores &e.synthAt into the
// enemy's damage collider and &e.synthStts into the synth; an unordered_map
// rehash would relocate live entries and dangle those pointers. The map is
// small (per-room whitelisted enemies); the node stability is the hardening
// and the same-frame consumption contract is documented on the members.
std::map<u16, EnemyEntry> g_entries;
std::unordered_map<fpc_ProcID, u16> g_pidToEnemyId;
u16 g_dynamicIdCounter = 0;  // per-owner low counter (owner-major id, M4)

// Client receive slots: latest snapshot per enemy id.
std::unordered_map<u16, net::EnemySnapshotMsg> g_received;

// Room-clear (03-enemies.md §5): the owner's per-room bit + which rooms had
// synced enemies (the client ALLDIE gate only applies to those rooms).
// Room numbers are s8 (0..127 in practice; negative rooms are skipped at
// registration), so the arrays are sized 2x the meaningful range for the
// sign byte; the RoomClear receive gate (< 128) keeps a malformed event
// from writing past the meaningful range (capstone MINOR 7).
bool g_roomClear[256] = {};
bool g_roomHadEnemies[256] = {};
s8 g_lastRoom = -2;
// Capstone MINOR D (review-full-deepseek-v4-flash-0731.md MINOR D): the
// per-stage reset keys on (stage, room), not the room number alone — a stage
// change whose room number coincides (F_SP103 room 1 -> F_SP104 room 1)
// previously relied on an implicit Link-less gap to reset. The stage name
// comes from the start-stage object (dComIfGp_getStartStageName).
char g_lastStage[net::kMaxStageNameLength] = {};
u32 g_frameCount = 0;

// Host room-clear event: per-room sent-once latch.
bool g_hostRoomClearSent = false;
s8 g_hostRoomClearRoom = -1;

struct PendingDied {
    u16 enemyId = kInvalidEnemyId;
    u8 drop = 0xFF;
    u8 sw = 0xFF;
};
std::vector<PendingDied> g_pendingDied;

bool SessionLive() {
    return dusk::coop::sessionActive();
}

/// M4 room ownership (network.md §6): this machine is the room owner of
/// `roomNo` in the CURRENT stage per the session ownership table. The room
/// owner sims that room's enemies natively (AI/HP/spawns), snapshots them,
/// and validates combat for them; every other player in the room sees frozen
/// puppets driven by those snapshots. v1-co-located host-role decisions all
/// become owner decisions here.
bool OwnsRoom(s8 roomNo) {
    if (!SessionLive()) {
        return false;
    }
    return dusk::coop::amIRoomOwner(roomNo);
}

const NetEnemyAdapter* AdapterForActor(const fopAc_ac_c* actor) {
    if (actor == nullptr) {
        return nullptr;
    }
    return findAdapter(fopAcM_GetName(const_cast<fopAc_ac_c*>(actor)));
}

u16 StageKey(const fopAc_ac_c* actor) {
    // Stage-placed key: (roomNo, setID) — identical on every machine for the
    // same stage, and STABLE across ownership transfers (the new owner
    // registers the same keys; clients' id-keyed maps never renumber). setID
    // values in TP rooms are small (< 0x100); larger setIDs (clones/waves)
    // fall back to the owner's dynamic counter (M5).
    return StageEntityId(fopAcM_GetRoomNo(actor), fopAcM_GetSetId(actor));
}

EnemyEntry* FindEntryForActor(const fopAc_ac_c* actor) {
    if (actor == nullptr) {
        return nullptr;
    }
    const auto it = g_pidToEnemyId.find(fopAcM_GetID(actor));
    if (it == g_pidToEnemyId.end()) {
        return nullptr;
    }
    const auto eit = g_entries.find(it->second);
    return eit != g_entries.end() ? &eit->second : nullptr;
}

void EraseEntry(u16 enemyId) {
    const auto it = g_entries.find(enemyId);
    if (it != g_entries.end()) {
        g_pidToEnemyId.erase(it->second.pid);
        g_entries.erase(it);
    }
}

void RegisterActor(fopAc_ac_c* actor, u16 enemyId) {
    if (actor == nullptr || enemyId == kInvalidEnemyId) {
        return;
    }
    if (g_entries.count(enemyId) != 0 || g_pidToEnemyId.count(fopAcM_GetID(actor)) != 0) {
        return;
    }
    auto [it, inserted] = g_entries.try_emplace(enemyId);
    if (!inserted) {
        return;
    }
    EnemyEntry& e = it->second;
    e.enemyId = enemyId;
    e.pid = fopAcM_GetID(actor);
    e.actor = actor;
    e.roomNo = fopAcM_GetRoomNo(actor);
    e.deathSwitch = 0xFF;
    // Capstone MAJOR 2: capture the per-type adapter at registration — the
    // death poll (and any later use) must never re-derive it from e.actor
    // once the actor can be freed. The kAdapters table is a static array
    // that outlives every entry, so holding the pointer is safe.
    const NetEnemyAdapter* adapter = AdapterForActor(actor);
    e.adapter = adapter;
    g_pidToEnemyId.emplace(e.pid, enemyId);
    if (e.roomNo >= 0) {
        g_roomHadEnemies[e.roomNo] = true;
    }
    EnemyLog.debug("enemy: registered {} {} enemyId=0x{:04X} room={} setID={}", e.pid,
        adapter != nullptr ? adapter->name : "?", enemyId, static_cast<s32>(e.roomNo),
        fopAcM_GetSetId(actor));
}

bool EligibleForRegistration(const fopAc_ac_c* actor) {
    if (actor == nullptr ||
        !isWhitelistedType(fopAcM_GetName(const_cast<fopAc_ac_c*>(actor))))
    {
        return false;
    }
    if (fopAcM_GetSetId(actor) == 0xFFFF) {
        return false;  // dynamic spawns: v1 stage-placed only
    }
    return true;
}

// ---------------------------------------------------------------------------
// Host: registration scan + death poll
// ---------------------------------------------------------------------------

int ScanActor(void* actorVoid, void* data) {
    fopAc_ac_c* actor = static_cast<fopAc_ac_c*>(actorVoid);
    (void)data;
    const s8 roomNo = fopAcM_GetRoomNo(actor);
    if (roomNo < 0) {
        return 1;
    }
    if (!EligibleForRegistration(actor)) {
        return 1;
    }
    if (g_pidToEnemyId.count(fopAcM_GetID(actor)) != 0) {
        return 1;
    }
    const u16 enemyId = StageKey(actor);
    if (enemyId == kInvalidEnemyId) {
        return 1;
    }
    RegisterActor(actor, enemyId);
    return 1;
}

void ScanAndRegister() {
    fopAcIt_Executor(ScanActor, nullptr);
}

void SendPendingDiedEvents() {
    for (const PendingDied& d : g_pendingDied) {
        net::PayloadUnion payload = {};
        payload.enemyEvent.enemyId = d.enemyId;
        payload.enemyEvent.eventId = static_cast<u8>(net::EnemyEventId::Died);
        payload.enemyEvent.data = d.drop;
        payload.enemyEvent.flagMask = d.sw;
        dusk::coop::sendGameMessage(net::MsgType::EnemyEvent, payload);
        EnemyLog.info("enemy: died event enemyId=0x{:04X} drop=0x{:02X} switch=0x{:02X}",
            d.enemyId, d.drop, d.sw);
    }
    g_pendingDied.clear();
}

/// Host per-frame: remove gone actors (died), refresh death-window captures.
/// Capstone MAJOR 2 (review-full-glm-5.2.md MAJOR 1): never deref e.actor
/// after it can be freed. The poll runs BEFORE fpcM_Management's delete pass
/// each frame, so on the frame the actor is gone the entry's actor pointer is
/// already dangling (freed by fpcDt_Handler the previous frame) — the
/// per-type data is read from e.adapter (captured at registration, safe: a
/// static table) and e.deathSwitch (captured while the actor still lived).
/// The only e.actor derefs left are the isDead/deathSwitchNo reads, which
/// are reached only when !gone (fopAcM_SearchByID(e.pid) == e.actor — the
/// actor is alive then).
void PollHostDeaths() {
    std::vector<u16> dead;
    for (auto& kv : g_entries) {
        EnemyEntry& e = kv.second;
        const bool gone = fopAcM_SearchByID(e.pid) != e.actor;
        if (gone) {
            if (!e.diedSent) {
                dead.push_back(e.enemyId);
            }
            continue;
        }
        // Death window (actor still alive but dying): capture the switch.
        if (e.adapter != nullptr && e.adapter->isDead != nullptr && e.adapter->isDead(e.actor)) {
            e.deathSwitch = e.adapter->deathSwitchNo != nullptr ? e.adapter->deathSwitchNo(e.actor) : 0xFF;
        }
    }
    for (u16 enemyId : dead) {
        auto it = g_entries.find(enemyId);
        if (it == g_entries.end()) {
            continue;
        }
        EnemyEntry& e = it->second;
        e.diedSent = true;
        // e.adapter is captured at registration (static table — valid even
        // though the actor is gone); e.deathSwitch was captured alive.
        const NetEnemyAdapter* adapter = e.adapter;
        PendingDied d;
        d.enemyId = enemyId;
        d.drop = adapter != nullptr ? adapter->dropTableId : 0xFF;
        d.sw = e.deathSwitch;
        g_pendingDied.push_back(d);
        EraseEntry(enemyId);
        EnemyLog.info("enemy: host enemy 0x{:04X} died (drop 0x{:02X} switch 0x{:02X})",
            enemyId, d.drop, d.sw);
    }
    SendPendingDiedEvents();
}

// ---------------------------------------------------------------------------
// Host: snapshot sender (post-execute, per frame)
// ---------------------------------------------------------------------------

// The anim hint is the model frame (u16) in v1; per-type action ids are
// private members. The client drives the same morf frame.
mDoExt_McaMorfSO* MorfOf(fopAc_ac_c* actor, u32 offset) {
    return *reinterpret_cast<mDoExt_McaMorfSO**>(reinterpret_cast<u8*>(actor) + offset);
}

u16 PackAnim(fopAc_ac_c* actor) {
    const NetEnemyAdapter* adapter = AdapterForActor(actor);
    if (adapter == nullptr) {
        return 0;
    }
    // m_modelMorf/mAnm_p/mpModelMorf2 are private (offsets pinned by the
    // class STATIC_ASSERTs); the rest are public members.
    switch (adapter->procName) {
    case fpcNm_E_AI_e: {
        auto* morf = MorfOf(actor, 0x5D0);
        return static_cast<u16>(morf != nullptr ? morf->getFrame() : 0.0f);
    }
    case fpcNm_E_HM_e: {
        auto* morf = MorfOf(actor, 0x618);
        return static_cast<u16>(morf != nullptr ? morf->getFrame() : 0.0f);
    }
    case fpcNm_E_DF_e: {
        auto* morf = static_cast<daE_DF_c*>(actor)->mpMorfSO;
        return static_cast<u16>(morf != nullptr ? morf->getFrame() : 0.0f);
    }
    case fpcNm_E_YC_e: {
        auto* morf = static_cast<e_yc_class*>(actor)->mpMorf;
        return static_cast<u16>(morf != nullptr ? morf->getFrame() : 0.0f);
    }
    case fpcNm_E_MD_e: {
        auto* morf = static_cast<daE_MD_c*>(actor)->mpModelMorf;
        return static_cast<u16>(morf != nullptr ? morf->getFrame() : 0.0f);
    }
    case fpcNm_B_TN_e: {
        auto* morf = MorfOf(actor, 0x600);
        return static_cast<u16>(morf != nullptr ? morf->getFrame() : 0.0f);
    }
    default:
        return 0;
    }
}

u8 PackFlags(fopAc_ac_c* actor) {
    u8 flags = 0;
    const NetEnemyAdapter* adapter = AdapterForActor(actor);
    if (adapter != nullptr && adapter->isDead != nullptr && adapter->isDead(actor)) {
        flags |= kSnapshotFlagDead;
    }
    return flags;
}

/// M4 same-room scoping (M2 review MINOR-6): the snapshot sender gate must
/// scope by the remotes' ACTUAL room (their PlayerState carries stage +
/// roomNo), not "any present player" — a room owner only streams to players
/// in the room it owns, so a client in another room never receives (nor
/// needs) this room's snapshots.
bool RemoteInRoom(s8 roomNo) {
    if (!SessionLive()) {
        return false;
    }
    return dusk::coop::remoteInRoom(roomNo);
}

void SendSnapshot(EnemyEntry& e, const NetEnemyAdapter* adapter) {
    fopAc_ac_c* actor = e.actor;
    const u16 hp = actor->health;
    const u16 maxHp = actor->field_0x560;
    const s16 angle = actor->shape_angle.y;
    const u32 anim = PackAnim(actor);
    const u8 flags = PackFlags(actor);
    if (e.lastFlags != 0xFF && e.lastPos[0] == actor->current.pos.x &&
        e.lastPos[1] == actor->current.pos.y && e.lastPos[2] == actor->current.pos.z &&
        e.lastSpeed[0] == actor->speed.x && e.lastSpeed[1] == actor->speed.y &&
        e.lastSpeed[2] == actor->speed.z && e.lastAngle == angle && e.lastHp == hp &&
        e.lastAnim == anim && e.lastFlags == flags)
    {
        return;  // dirty-flag: unchanged enemies are skipped
    }
    e.lastPos[0] = actor->current.pos.x;
    e.lastPos[1] = actor->current.pos.y;
    e.lastPos[2] = actor->current.pos.z;
    e.lastSpeed[0] = actor->speed.x;
    e.lastSpeed[1] = actor->speed.y;
    e.lastSpeed[2] = actor->speed.z;
    e.lastAngle = angle;
    e.lastHp = hp;
    e.lastAnim = anim;
    e.lastFlags = flags;

    net::PayloadUnion payload = {};
    net::EnemySnapshotMsg& snap = payload.enemySnapshot;
    snap.enemyId = e.enemyId;
    snap.type = fopAcM_GetName(actor);
    snap.hp = hp;
    snap.maxHp = maxHp;
    // Aggro hint = the nearest real player (same resolution the targeting
    // context uses; D3). 0 = the host's Link, else the remote puppet's id.
    fopAc_ac_c* nearest = dusk::coop::resolveNearestPlayer(actor->current.pos);
    net::PlayerId aggroId = kInvalidPlayerId;
    if (nearest != nullptr) {
        aggroId = dusk::coop::puppetPlayerId(fopAcM_GetID(nearest));
        if (aggroId == kInvalidPlayerId) {
            aggroId = 0;  // the host's own Link
        }
    }
    snap.aggro = aggroId;
    snap.semantics = static_cast<u8>(adapter->semantics);
    snap.flags = flags;
    snap.angle = angle;
    snap.anim = anim;
    snap.pos.x = actor->current.pos.x;
    snap.pos.y = actor->current.pos.y;
    snap.pos.z = actor->current.pos.z;
    snap.speed.x = actor->speed.x;
    snap.speed.y = actor->speed.y;
    snap.speed.z = actor->speed.z;
    dusk::coop::sendGameMessage(net::MsgType::EnemySnapshot, payload);
}

// ---------------------------------------------------------------------------
// Host: synthetic hit injection (m2-design-notes.md §1)
// ---------------------------------------------------------------------------

/// Builds the synthetic full At collider carrying the intent's raw attack
/// fields and sets the Tg hit flag on the enemy's damage collider. The
/// enemy's own next execute polls ChkTgHit() and runs its authentic damage
/// reaction. A raw cCcD_Obj returns NULL from GetGObjInf() and crashes
/// at_power_check, so the object is a full dCcD_Sph with its dCcD_GObjInf
/// populated.
void SetTgHitSynthetic(EnemyEntry& e, cCcD_ObjHitInf* damageCollider,
                       const net::CombatIntentMsg& intent) {
    if (damageCollider == nullptr) {
        return;
    }
    fopAc_ac_c* attacker = dusk::coop::puppetActorFor(intent.attackerId);
    if (attacker == nullptr) {
        attacker = dComIfGp_getPlayer(0);  // host Link fallback (local hit)
    }

    e.synthStts.SetActor(attacker != nullptr ? attacker : e.actor);
    e.synthAt.SetStts(&e.synthStts);
    e.synthAt.SetAtAtp(intent.atp);
    e.synthAt.SetAtType(intent.atType);
    dCcD_GObjInf* ginfo = static_cast<dCcD_GObjInf*>(e.synthAt.GetGObjInf());
    if (ginfo != nullptr) {
        ginfo->SetAtSe(1);  // dCcD_SE_SWORD
        ginfo->SetAtMtrl(dCcD_MTRL_NONE);
        ginfo->SetAtHitMark(1);
    }
    cXyz hitPos(intent.hitPos.x, intent.hitPos.y, intent.hitPos.z);
    e.synthAt.SetAtHitPos(hitPos);
    // Hit-position consumers on the enemy side read the contact point from
    // the ENEMY's collider (the real collision pass sets it; the injection
    // must too, or hitmarks land at a stale/zero spot).
    if (dCcD_GObjInf* g = static_cast<dCcD_GObjInf*>(damageCollider)) {
        g->SetTgHitPos(hitPos);
    }

    // The enemy's own handler re-binds mAtInfo.mpCollider from GetTgHitObj(),
    // so the only AtInfo field the injection must pre-seed is the power type
    // — which the enemy's own create set from the same value the client used
    // (the adapter's defaultPowerType contract).
    damageCollider->SetTgHit(&e.synthAt);
}

// m_ccCyl is private in e_ai_class; the offset comes from its STATIC_ASSERT-
// pinned layout (d_a_e_ai.h: /* 0xBC8 */ dCcD_Cyl m_ccCyl).
void aiInjectHit(fopAc_ac_c* actor, const net::CombatIntentMsg& intent) {
    if (EnemyEntry* e = FindEntryForActor(actor)) {
        auto* collider = reinterpret_cast<cCcD_ObjHitInf*>(
            reinterpret_cast<u8*>(actor) + 0xBC8);
        SetTgHitSynthetic(*e, collider, intent);
    }
}
// mSph is private in daE_HM_c (d_a_e_hm.h: /* 0x928 */ dCcD_Sph mSph).
void hmInjectHit(fopAc_ac_c* actor, const net::CombatIntentMsg& intent) {
    if (EnemyEntry* e = FindEntryForActor(actor)) {
        auto* collider = reinterpret_cast<cCcD_ObjHitInf*>(
            reinterpret_cast<u8*>(actor) + 0x928);
        SetTgHitSynthetic(*e, collider, intent);
    }
}
void dfInjectHit(fopAc_ac_c* actor, const net::CombatIntentMsg& intent) {
    if (EnemyEntry* e = FindEntryForActor(actor)) {
        SetTgHitSynthetic(*e, &static_cast<daE_DF_c*>(actor)->mCyl, intent);
    }
}
void ycInjectHit(fopAc_ac_c* actor, const net::CombatIntentMsg& intent) {
    if (EnemyEntry* e = FindEntryForActor(actor)) {
        SetTgHitSynthetic(*e, &static_cast<e_yc_class*>(actor)->mCcSph, intent);
    }
}
void mdInjectHit(fopAc_ac_c* actor, const net::CombatIntentMsg& intent) {
    if (EnemyEntry* e = FindEntryForActor(actor)) {
        SetTgHitSynthetic(*e, &static_cast<daE_MD_c*>(actor)->mCyl, intent);
    }
}
void tnInjectHit(fopAc_ac_c* actor, const net::CombatIntentMsg& intent) {
    // The Darknut's damage_check polls mSphC then mSphA[0..2] then mSphB[0..2];
    // set the flag on all of them — its own routing picks the first hit.
    EnemyEntry* e = FindEntryForActor(actor);
    if (e == nullptr) {
        return;
    }
    // mSphA/mSphB/mSphC are private in daB_TN_c (d_a_b_tn.h: mSphA 0x2EC8,
    // mSphB 0x3270, mSphC 0x3618; each dCcD_Sph is 0x138).
    auto sph = [](fopAc_ac_c* a, u32 off) {
        return reinterpret_cast<cCcD_ObjHitInf*>(reinterpret_cast<u8*>(a) + off);
    };
    SetTgHitSynthetic(*e, sph(actor, 0x3618), intent);  // mSphC
    for (int i = 0; i < 3; ++i) {
        SetTgHitSynthetic(*e, sph(actor, 0x2EC8 + i * 0x138), intent);  // mSphA
        SetTgHitSynthetic(*e, sph(actor, 0x3270 + i * 0x138), intent);  // mSphB
    }
}

// ---------------------------------------------------------------------------
// Client: died handling (drops + death beat + per-player switch, D5/D9)
// ---------------------------------------------------------------------------

void GrantLocalSwitch(u8 sw, s8 roomNo) {
    if (sw == 0xFF || roomNo < 0) {
        return;
    }
    if (!dComIfGs_isSwitch(sw, roomNo)) {
        dComIfGs_onSwitch(sw, roomNo);
        EnemyLog.info("enemy: granted local switch {} in room {}", static_cast<u32>(sw),
            static_cast<s32>(roomNo));
    }
}

void SpawnDropAndBeat(fopAc_ac_c* actor, const NetEnemyAdapter* adapter, u8 dropTableId) {
    if (actor == nullptr) {
        return;
    }
    // Play-scene layer trick (local-coop-poc spawnClone pattern): the pump
    // runs pre-execute; the item/disappear creates need a valid current layer.
    layer_class* saved = fpcLy_CurrentLayer();
    base_process_class* proc = reinterpret_cast<base_process_class*>(actor);
    if (proc->layer_tag.layer != nullptr) {
        fpcLy_SetCurrentLayer(proc->layer_tag.layer);
    }
    if (dropTableId != 0xFF) {
        fopAcM_createItemFromEnemyID(dropTableId, &actor->current.pos, -1, -1, NULL, NULL, NULL,
            NULL);
        EnemyLog.info("enemy: client spawned drop table 0x{:02X} for 0x{:04X}", dropTableId,
            entityIdForActor(actor));
    }
    // Death VFX/SFX beat (enemy id 0xFF: no second drop from the disappear
    // actor — the drop above is explicit, D5).
    const u8 beatSize = adapter != nullptr && adapter->procName == fpcNm_E_AI_e ? 12 : 10;
    fopAcM_createDisappear(actor, &actor->current.pos, beatSize, 0, 0xFF);
    fpcLy_SetCurrentLayer(saved);
}

void OnEnemyDied(u16 enemyId, u8 dropTableId, u8 flagMask) {
    auto it = g_entries.find(enemyId);
    if (it == g_entries.end()) {
        return;  // never registered (non-whitelisted) or already gone
    }
    EnemyEntry& e = it->second;
    if (e.state == EntryState::Dying) {
        return;
    }
    const NetEnemyAdapter* adapter = AdapterForActor(e.actor);
    if (e.actor != nullptr) {
        SpawnDropAndBeat(e.actor, adapter, dropTableId);
        GrantLocalSwitch(flagMask, e.roomNo);
    }
    e.state = EntryState::Dying;
    e.dyingFrame = g_frameCount;
    g_received.erase(enemyId);
    EnemyLog.info("enemy: client puppet 0x{:04X} dying (drop 0x{:02X} switch 0x{:02X})", enemyId,
        dropTableId, flagMask);
}

void ClearAll() {
    g_entries.clear();
    g_pidToEnemyId.clear();
    g_received.clear();
    std::memset(g_roomClear, 0, sizeof(g_roomClear));
    std::memset(g_roomHadEnemies, 0, sizeof(g_roomHadEnemies));
    g_pendingDied.clear();
    g_hostRoomClearSent = false;
    g_hostRoomClearRoom = -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public: adapter lookup
// ---------------------------------------------------------------------------

const NetEnemyAdapter* findAdapter(s16 procName) {
    for (const auto& a : kAdapters) {
        if (a.procName == procName) {
            return &a;
        }
    }
    return nullptr;
}

bool isWhitelistedType(s16 procName) {
    return findAdapter(procName) != nullptr;
}

// ---------------------------------------------------------------------------
// Public: registry
// ---------------------------------------------------------------------------

u16 entityIdForActor(const fopAc_ac_c* actor) {
    if (actor == nullptr) {
        return kInvalidEnemyId;
    }
    const auto it = g_pidToEnemyId.find(fopAcM_GetID(actor));
    if (it != g_pidToEnemyId.end()) {
        return it->second;
    }
    return kInvalidEnemyId;
}

fopAc_ac_c* actorForEntityId(u16 enemyId) {
    const auto it = g_entries.find(enemyId);
    if (it == g_entries.end()) {
        return nullptr;
    }
    return it->second.actor;
}

bool isRegistered(const fopAc_ac_c* actor) {
    return FindEntryForActor(actor) != nullptr;
}

u16 registerDynamicEnemy(fopAc_ac_c* actor) {
    if (actor == nullptr || isRegistered(actor)) {
        return kInvalidEnemyId;
    }
    // Owner-assigned monotonic counter, OWNER-MAJOR (coop_entity_logic.h):
    // the top bit keeps dynamic ids out of the stage-key space, the upper
    // nibble carries the owning PlayerId, and the low counter wraps per
    // owner — after an ownership transfer the new owner's dynamic ids can
    // never collide with the previous owner's (entity-id stability, M4).
    // Clients learn dynamic ids via EnemyEvent(spawn) (v1 co-located is
    // stage-placed only, so this path is dormant).
    g_dynamicIdCounter = (g_dynamicIdCounter + 1) & kDynamicCounterMask;
    const u16 enemyId = DynamicEntityId(dusk::coop::selfId(), g_dynamicIdCounter);
    RegisterActor(actor, enemyId);
    return enemyId;
}

void applyInjectedHit(fopAc_ac_c* actor, const net::CombatIntentMsg& intent) {
    const NetEnemyAdapter* adapter = AdapterForActor(actor);
    if (adapter == nullptr || adapter->injectHit == nullptr) {
        return;
    }
    adapter->injectHit(actor, intent);
    // Latch the result: the enemy's own execute this frame consumes the hit;
    // hostOnExecuted sends the accurate CombatResult post-execute.
    if (EnemyEntry* e = FindEntryForActor(actor)) {
        e->pendingHit = true;
        e->hitDamage = intent.computedPower;
        e->hitSeq = intent.seq;
        e->hitAttacker = intent.attackerId;
    }
}

// ---------------------------------------------------------------------------
// Public: fopAc_Execute hooks
// ---------------------------------------------------------------------------

bool puppetExecute(fopAc_ac_c* actor) {
    if (!SessionLive() || actor == nullptr) {
        return false;
    }
    // M4: the ROOM OWNER's enemies run natively (this machine sims them);
    // only non-owner machines freeze + drive puppets.
    const s8 roomNo = fopAcM_GetRoomNo(actor);
    if (OwnsRoom(roomNo)) {
        return false;
    }
    EnemyEntry* e = FindEntryForActor(actor);
    if (e == nullptr) {
        // Capstone MAJOR 3 (review-full-glm-5.2.md MAJOR 2): freeze
        // OPTIMISTICALLY for whitelisted types in a non-owned room before
        // registration lands. The room-change scan registers them on the
        // first frame, but a mid-room spawn or a scan miss would otherwise
        // let the local instance run its full native AI (aggro, move, attack
        // — real damage to the client's Link, bypassing the CombatIntent
        // path). Holding the spawn pose until the first snapshot arrives is
        // exactly the v1 freeze semantics.
        if (isWhitelistedType(fopAcM_GetName(const_cast<fopAc_ac_c*>(actor)))) {
            actor->old = actor->current;
            return true;
        }
        return false;
    }
    if (e->state == EntryState::Dying) {
        // Death beat: hold the last pose (no further snapshots arrive anyway).
        actor->old = actor->current;
        return true;
    }
    const auto it = g_received.find(e->enemyId);
    if (it == g_received.end()) {
        return true;  // no snapshot yet — hold the spawn pose
    }
    const net::EnemySnapshotMsg& snap = it->second;
    const NetEnemyAdapter* adapter = AdapterForActor(actor);
    if (adapter == nullptr) {
        return true;
    }

    // Base fields (03-enemies.md §3.2).
    actor->current.pos.x = snap.pos.x;
    actor->current.pos.y = snap.pos.y;
    actor->current.pos.z = snap.pos.z;
    actor->current.angle.y = snap.angle;
    actor->shape_angle.y = snap.angle;
    actor->speed.x = snap.speed.x;
    actor->speed.y = snap.speed.y;
    actor->speed.z = snap.speed.z;
    actor->health = snap.hp;
    actor->field_0x560 = snap.maxHp;

    // fopEn_enemy_c flag subset (dead/downed/wolf-bitten) — mFlags is public
    // on the enemy base. v1 carries only the dead bit; keep the rest of the
    // word untouched.
    fopEn_enemy_c* enemyBase = static_cast<fopEn_enemy_c*>(actor);
    if (snap.flags & kSnapshotFlagDead) {
        enemyBase->mFlags |= static_cast<u16>(fopEn_enemy_c::fopEn_flag_Dead);
    } else {
        enemyBase->mFlags &= static_cast<u16>(~fopEn_enemy_c::fopEn_flag_Dead);
    }

    // Per-type drive: model matrix + anim frame, then collider re-registration
    // (dCcS clears registrations every frame — 03-enemies.md §1.5).
    adapter->driveModel(actor, snap);
    adapter->refreshColliders(actor);

    // old = current after apply (physics/interp consumers see a coherent
    // delta); cullMtx from current.pos (plan risk 10: stale cullMtx when
    // fopAcStts_CULL_e is set on the profile status word — E_AI/E_HM/E_DF/
    // E_YC/E_MD all set it, and a frozen puppet never refreshes it). Stored
    // in the per-entry member so multi-puppet rooms each cull on their own
    // matrix (M2.5, MAJOR-2).
    actor->old = actor->current;
    mDoMtx_stack_c::transS(actor->current.pos);
    std::memcpy(e->cullMtx, mDoMtx_stack_c::get(), sizeof(Mtx));
    fopAcM_SetMtx(actor, e->cullMtx);
    return true;
}

bool hostNeedsContext(const fopAc_ac_c* actor) {
    if (!SessionLive() || actor == nullptr) {
        return false;
    }
    // M4: the ROOM OWNER's enemies chase the nearest real player (D3 context
    // swap); non-owner machines never push (their enemies are frozen).
    if (!OwnsRoom(fopAcM_GetRoomNo(actor))) {
        return false;
    }
    return isRegistered(actor) && AdapterForActor(actor) != nullptr;
}

void hostOnExecuted(fopAc_ac_c* actor) {
    if (!SessionLive() || actor == nullptr) {
        return;
    }
    EnemyEntry* e = FindEntryForActor(actor);
    if (e == nullptr) {
        return;
    }
    // M4: only the room owner snapshots/death-polls the enemy. (The owner is
    // in the room it owns, so e->roomNo == the local room on the owner; the
    // check is belt-and-braces.)
    if (!OwnsRoom(e->roomNo)) {
        return;
    }
    const NetEnemyAdapter* adapter = AdapterForActor(actor);
    if (adapter == nullptr) {
        return;
    }
    // Death-window capture while the actor still lives (the switch must be
    // read before the delete; the died event fires when the actor is gone).
    if (adapter->isDead != nullptr && adapter->isDead(actor) &&
        adapter->deathSwitchNo != nullptr)
    {
        e->deathSwitch = adapter->deathSwitchNo(actor);
    }
    if (e->roomNo != dusk::coop::localRoomNo()) {
        return;
    }
    // M4 same-room scoping: only stream to remotes actually in this room.
    if (RemoteInRoom(e->roomNo)) {
        SendSnapshot(*e, adapter);
    }
    // Post-execute CombatResult for an injected hit (accurate HP now that the
    // enemy's own handler ran). Best-effort Died outcome; the authoritative
    // death signal is EnemyEvent(died) from the death poll.
    if (e->pendingHit) {
        e->pendingHit = false;
        net::PayloadUnion payload = {};
        payload.combatResult.targetEnemyId = e->enemyId;
        payload.combatResult.damage = e->hitDamage;
        payload.combatResult.newHp = actor->health;
        payload.combatResult.outcome = static_cast<u8>(actor->health <= 0 ? net::CombatOutcome::Died
                                                                          : net::CombatOutcome::Hit);
        payload.combatResult.attackerId = e->hitAttacker;
        payload.combatResult.seq = e->hitSeq;
        dusk::coop::sendGameMessage(net::MsgType::CombatResult, payload);
    }
}

// ---------------------------------------------------------------------------
// Public: game-message routing (client side)
// ---------------------------------------------------------------------------

void onGameMessage(net::MsgType type, const net::PayloadUnion& payload) {
    if (!SessionLive()) {
        return;
    }
    switch (type) {
    case net::MsgType::EnemySnapshot: {
        const auto& snap = payload.enemySnapshot;
        if (snap.enemyId == kInvalidEnemyId) {
            return;
        }
        // M4 same-room scoping (defense in depth): a stage-placed id decodes
        // to its room ((roomNo << 8) | setID); drop snapshots for rooms we
        // are not in — the host already scopes the relay to the sender's
        // room, this catches any leaked/forged copy. Dynamic ids (>= the
        // dynamic base) don't decode — accepted (M5 dynamic waves).
        if (snap.enemyId < kDynamicIdBase &&
            static_cast<s8>(snap.enemyId >> 8) != dusk::coop::localRoomNo())
        {
            return;
        }
        g_received[snap.enemyId] = snap;
        // Snapshot arrival also (re)establishes the client-side puppet for
        // stage-placed enemies: scan once so the freeze/apply picks it up.
        if (g_pidToEnemyId.empty()) {
            ScanAndRegister();
        }
        break;
    }
    case net::MsgType::EnemyEvent: {
        const auto& ev = payload.enemyEvent;
        // M4.5 (MAJOR 2 — defense in depth; the relay is room-scoped now):
        // only events for THE room the client is in may act. Died carries a
        // stage-placed id ((roomNo << 8) | setID) — a cross-stage peer with a
        // coincident id would mis-kill a local enemy, spawn the wrong drop,
        // grant a wrong save switch and empty its ALLDIE scan early. RoomClear
        // carries the BARE room number. Events for any other room are dropped.
        switch (static_cast<net::EnemyEventId>(ev.eventId)) {
        case net::EnemyEventId::Died:
            if (ev.enemyId < kDynamicIdBase &&
                static_cast<s8>(ev.enemyId >> 8) != dusk::coop::localRoomNo())
            {
                return;  // another room's enemy (or a leaked cross-stage copy)
            }
            OnEnemyDied(ev.enemyId, static_cast<u8>(ev.data & 0xFF), ev.flagMask);
            break;
        case net::EnemyEventId::RoomClear:
            // Capstone MINOR 7 (review-full-glm-5.2.md MINOR 7): room numbers
            // are s8 (0..127 in practice); the old < 256 gate let a forged
            // RoomClear write g_roomClear[128..255], slots no real s8 room
            // ever reads. Tightened to < 128 (the arrays stay 256 — 2x the
            // meaningful range for the s8 sign).
            if (ev.enemyId < 128 &&
                static_cast<s8>(ev.enemyId) == dusk::coop::localRoomNo())
            {
                g_roomClear[ev.enemyId] = true;
                EnemyLog.info("enemy: room-clear bit for room {}", static_cast<s32>(ev.enemyId));
            }
            break;
        case net::EnemyEventId::Spawned:
            // v1 co-located is stage-placed only — the owner never sends
            // dynamic spawns. Logged rather than created (the client cannot
            // reproduce owner params; M5 dynamic waves).
            EnemyLog.debug("enemy: ignoring dynamic spawn event 0x{:04X} (v1 stage-placed)",
                ev.enemyId);
            break;
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Public: room-clear (d_a_alldie.cpp hooks)
// ---------------------------------------------------------------------------

bool clientRoomClearGated(s8 roomNo) {
    if (!SessionLive() || roomNo < 0) {
        return false;
    }
    // M4: the ROOM OWNER's ALLDIE scan is authoritative (its enemies died
    // natively); only non-owner machines gate on the owner's roomClear bit.
    if (OwnsRoom(roomNo)) {
        return false;
    }
    // Gate only rooms that had synced enemies: their death mirror must be
    // authoritative (the died events deleted the puppets), and the owner's
    // per-room bit is the backstop for corpse-linger / dynamic-spawn edges
    // (03-enemies.md §5.2).
    if (!g_roomHadEnemies[roomNo]) {
        return false;
    }
    return !g_roomClear[roomNo];
}

void hostRoomCleared(s8 roomNo) {
    if (!SessionLive()) {
        return;
    }
    if (roomNo < 0) {
        return;
    }
    // M4: the room owner's vanilla scan coming up empty is the authoritative
    // room-clear (the bit keeps clients' ALLDIE scans honest).
    if (!OwnsRoom(roomNo)) {
        return;
    }
    if (g_hostRoomClearSent && g_hostRoomClearRoom == roomNo) {
        return;  // once per room
    }
    g_hostRoomClearSent = true;
    g_hostRoomClearRoom = roomNo;
    net::PayloadUnion payload = {};
    payload.enemyEvent.enemyId = static_cast<u16>(roomNo);
    payload.enemyEvent.eventId = static_cast<u8>(net::EnemyEventId::RoomClear);
    dusk::coop::sendGameMessage(net::MsgType::EnemyEvent, payload);
    EnemyLog.info("enemy: host room {} cleared — roomClear bit sent", static_cast<s32>(roomNo));
}

// ---------------------------------------------------------------------------
// Public: per-frame pump
// ---------------------------------------------------------------------------

void onGameFrame() {
    ++g_frameCount;

    if (!SessionLive()) {
        if (!g_entries.empty() || g_lastRoom != -2) {
            ClearAll();
            g_lastRoom = -2;
        }
        return;
    }

    // Room-change detection -> per-stage state reset. Capstone MINOR D: the
    // reset keys on (stage, room), not the room number alone — a stage change
    // whose room number coincides (F_SP103 room 1 -> F_SP104 room 1)
    // previously relied on an implicit Link-less gap (LocalRoomNo() == -1 for
    // >= 1 frame) to reset; keying on the stage name + room makes the reset
    // explicit, so a future change that keeps the Link alive across a stage
    // transition cannot carry stale (room<<8)|setID entries into the new
    // stage (which would make RegisterActor refuse the new stage's
    // coincident-id enemies and de-frost them).
    const s8 roomNow = dusk::coop::localRoomNo();
    const char* stageNow = dusk::coop::localStageName();
    const bool stageChanged = std::strcmp(stageNow, g_lastStage) != 0;
    if (stageChanged || roomNow != g_lastRoom) {
        if (g_lastRoom != -2) {
            EnemyLog.info("enemy: room change {} -> {} (stage '{}' -> '{}'); resetting per-stage state",
                static_cast<s32>(g_lastRoom), static_cast<s32>(roomNow), g_lastStage, stageNow);
            ClearAll();
        }
        std::snprintf(g_lastStage, sizeof(g_lastStage), "%s", stageNow);
        g_lastRoom = roomNow;
        // Capstone MAJOR 3 (review-full-glm-5.2.md MAJOR 2): register
        // IMMEDIATELY on the room change (NOT gated on the % 15 scan cadence)
        // so local instances are registered — and thus frozen by
        // puppetExecute — on the FIRST frame in the new room. A non-owner's
        // whitelisted enemies would otherwise run full native AI (real damage
        // to the client's Link, bypassing the intent path) until the first
        // 15-frame scan. The 15-frame cadence stays as a backstop for
        // mid-room spawns.
        ScanAndRegister();
    }

    if (OwnsRoom(roomNow)) {
        // ROOM OWNER: registration scan (stage-placed whitelisted enemies in
        // the current room), native death poll, and combat-intent injection
        // before the actor phase (the enemy's execute then polls the injected
        // Tg hit flag and runs its authentic damage reaction). The snapshot
        // sender runs post-execute in hostOnExecuted, owner-gated.
        if (g_frameCount % 15 == 0) {
            ScanAndRegister();
        }
        PollHostDeaths();
        combat::flushHostIntents();
    } else {
        // NON-OWNER: register local instances as freeze targets; delete
        // puppets whose death beat finished.
        if (g_frameCount % 15 == 0) {
            ScanAndRegister();
        }
        for (auto it = g_entries.begin(); it != g_entries.end();) {
            EnemyEntry& e = it->second;
            if (e.state == EntryState::Dying && g_frameCount - e.dyingFrame >= 18) {
                if (e.actor != nullptr) {
                    fopAcM_delete(e.actor);
                }
                it = g_entries.erase(it);
                continue;
            }
            ++it;
        }
    }
}

void shutdown() {
    ClearAll();
    g_lastRoom = -2;
    g_lastStage[0] = '\0';
}

}  // namespace dusk::coop::enemy
