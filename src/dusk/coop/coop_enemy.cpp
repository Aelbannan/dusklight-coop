#include "dusk/coop/coop_enemy.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_difficulty.h"
#include "dusk/coop/coop_drops.h"

#include <algorithm>
#include <vector>

#if TARGET_PC
#include "SSystem/SComponent/c_bg_w.h"
#include "SSystem/SComponent/c_m3d.h"
#include "SSystem/SComponent/c_m3d_g_pla.h"
#include "SSystem/SComponent/c_math.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_iter.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "f_pc/f_pc_node.h"
#include "m_Do/m_Do_mtx.h"
#endif

namespace dusk::coop::enemy {
namespace {

std::vector<EnemyAdapter> g_adapters;
u32 g_pendingWaves = 0;

#if TARGET_PC

// Ground codes Link treats as void / exit / fog death (unsafe placement).
constexpr int kHazardGroundCodes[] = {4, 5, 9, 10};

std::vector<fpc_ProcID> g_pendingIds;
std::vector<fpc_ProcID> g_liveCloneIds;
std::vector<fpc_ProcID> g_augmentedSources;
std::vector<fpc_ProcID> g_notedSources;

bool containsId(const std::vector<fpc_ProcID>& ids, fpc_ProcID id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void eraseId(std::vector<fpc_ProcID>& ids, fpc_ProcID id) {
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
}

s16 actorProcName(fopAc_ac_c* actor) {
    return actor != nullptr ? fopAcM_GetName(actor) : static_cast<s16>(-1);
}

u32 clonesForParty() {
    // Gate H: use snapshotted count mul when an encounter is active.
    if (drops::encounterActive()) {
        return drops::clonesPerEligibleSource();
    }
    // Before snapshot: Gate G fallback — one clone only when a second player joined.
    return runtime().joinedPlayerCount >= 2 ? 1u : 0u;
}

void applySnapshottedHp(fopAc_ac_c* actor) {
    if (actor == nullptr || !drops::encounterActive()) {
        return;
    }
    const difficulty::Q16_16 hpMul = drops::currentEncounter().perEnemyHpMul;
    if (hpMul == difficulty::ONE) {
        return;
    }
    // field_0x560 is max/base; health is current. Scale both from the vanilla base.
    const s16 base = actor->field_0x560 > 0 ? actor->field_0x560 : actor->health;
    if (base <= 0) {
        return;
    }
    const s16 scaled = difficulty::scaleEnemyHealth(base, hpMul);
    actor->field_0x560 = scaled;
    actor->health = scaled;
}

u64 encounterSeedForRoom() {
    // Deterministic-enough for PoC: party + profile + eligible source count.
    // Same snapshot inputs → same seed via makeSnapshot call site.
    return (static_cast<u64>(runtime().joinedPlayerCount) << 32) |
           (static_cast<u64>(difficulty::currentProfile()) << 16) |
           static_cast<u64>(g_notedSources.size() + g_augmentedSources.size());
}

// Armos (E_AI) parameter map — see gate_G_enemy_adapter.md.
//   bits  0-7 : home_distance factor (field_0x5ba); keep
//   bits  8-15: unused by Create; keep verbatim
//   bits 16-23: death switch (m_swbit); sanitize → 0xFF (none)
//   bits 24-31: unused by Create; keep verbatim
u32 sanitizeArmosParameters(u32 params) {
    return (params & ~0x00FF0000u) | 0x00FF0000u;
}

u32 sanitizeParameters(s16 procName, u32 params, const EnemyAdapter& adapter) {
    if (adapter.allowProgressionSwitch) {
        return params;
    }
    if (procName == fpcNm_E_AI_e) {
        return sanitizeArmosParameters(params);
    }
    // Unknown whitelisted enemy: strip mid-byte switch as a safe default.
    return (params & ~0x00FF0000u) | 0x00FF0000u;
}

bool isHazardGround() {
    const int code = dComIfG_Bgsp().GetGroundCode(*fopAcM_gc_c::getGroundCheck());
    for (int hazard : kHazardGroundCodes) {
        if (code == hazard) {
            return true;
        }
    }
    return false;
}

bool hasCapsuleClearance(const cXyz& pos, f32 radius, f32 height, const fopAc_ac_c* ignore) {
    // Roof: enough headroom for the body capsule.
    cXyz roofProbe = pos;
    roofProbe.y += 10.0f;
    if (fopAcM_rc_c::roofCheck(&roofProbe)) {
        if (fopAcM_rc_c::getRoofY() - pos.y < height) {
            return false;
        }
    }

    // Horizontal: four cardinal probes at mid-height must not hit walls inside radius.
    static const cXyz kDirs[] = {
        {1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f},
    };
    const cXyz start(pos.x, pos.y + height * 0.5f, pos.z);
    for (const cXyz& dir : kDirs) {
        cXyz end = start;
        end.x += dir.x * radius;
        end.z += dir.z * radius;
        if (fopAcM_lc_c::lineCheck(&start, &end, ignore) && fopAcM_lc_c::checkWallHit()) {
            return false;
        }
    }
    return true;
}

bool tryPlaceClone(const fopAc_ac_c* source, const EnemyAdapter& adapter, u32 ordinal, cXyz* outPos,
                   csXyz* outAngle) {
    if (source == nullptr || outPos == nullptr || outAngle == nullptr) {
        return false;
    }

    // Deterministic offset ring around home (stable even if the statue has wandered).
    static const cXyz kLocalOffsets[] = {
        {1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f}, {0.7071f, 0.0f, 0.7071f}, {-0.7071f, 0.0f, -0.7071f},
        {0.7071f, 0.0f, -0.7071f}, {-0.7071f, 0.0f, 0.7071f},
    };

    const f32 dist = adapter.spawnOffset * static_cast<f32>(1 + ordinal);
    const s16 yaw = source->home.angle.y;
    const size_t startIdx = ordinal % (sizeof(kLocalOffsets) / sizeof(kLocalOffsets[0]));

    for (size_t n = 0; n < sizeof(kLocalOffsets) / sizeof(kLocalOffsets[0]); ++n) {
        const cXyz& localUnit = kLocalOffsets[(startIdx + n) % (sizeof(kLocalOffsets) / sizeof(kLocalOffsets[0]))];
        cXyz local = localUnit;
        local.x *= dist;
        local.z *= dist;

        cXyz world;
        mDoMtx_stack_c::YrotS(yaw);
        mDoMtx_stack_c::multVec(&local, &world);
        world += source->home.pos;
        world.y += 50.0f;

        if (!fopAcM_gc_c::gndCheck(&world)) {
            continue;
        }
        const f32 groundY = fopAcM_gc_c::getGroundY();
        if (groundY <= -G_CM3D_F_INF) {
            continue;
        }
        world.y = groundY;

        if (isHazardGround()) {
            continue;
        }

        cM3dGPla pla;
        if (fopAcM_gc_c::getTriPla(&pla) && !cBgW_CheckBGround(pla.mNormal.y)) {
            continue;
        }

        // Reject deep water over the candidate foot position.
        if (fopAcM_wt_c::waterCheck(&world)) {
            const f32 waterY = fopAcM_wt_c::getWaterY();
            if (waterY > groundY + 40.0f) {
                continue;
            }
        }

        if (!hasCapsuleClearance(world, adapter.capsuleRadius, adapter.capsuleHeight, source)) {
            continue;
        }

        *outPos = world;
        *outAngle = source->home.angle;
        outAngle->y = yaw + static_cast<s16>(0x2000 * static_cast<s32>(ordinal + n));
        return true;
    }

    return false;
}

void resolvePending() {
    for (auto it = g_pendingIds.begin(); it != g_pendingIds.end();) {
        const fpc_ProcID id = *it;
        if (fpcM_IsCreating(id)) {
            ++it;
            continue;
        }
        fopAc_ac_c* actor = fopAcM_SearchByID(id);
        if (actor == nullptr) {
            debug::logWarn("enemy: pending clone pid=%u failed create", static_cast<unsigned>(id));
            it = g_pendingIds.erase(it);
            continue;
        }
        if (!containsId(g_liveCloneIds, id)) {
            g_liveCloneIds.push_back(id);
            applySnapshottedHp(actor);
            debug::logInfo("enemy: clone live pid=%u name=%d hp=%d", static_cast<unsigned>(id),
                           fopAcM_GetName(actor), actor->health);
        }
        it = g_pendingIds.erase(it);
    }
}

void pruneDeadClones() {
    for (auto it = g_liveCloneIds.begin(); it != g_liveCloneIds.end();) {
        if (fopAcM_SearchByID(*it) == nullptr) {
            it = g_liveCloneIds.erase(it);
        } else {
            ++it;
        }
    }
}

void processNotedSources() {
    // Gate H: snapshot once when eligible originals are first seen this room.
    if (!g_notedSources.empty() && !drops::encounterActive()) {
        // Count all known eligible originals (noted + already augmented).
        u32 originals = static_cast<u32>(g_notedSources.size() + g_augmentedSources.size());
        // Also count live stage enemies of whitelisted types already present.
        for (fpc_ProcID id : g_notedSources) {
            (void)id;
        }
        drops::ensureEncounter(originals, encounterSeedForRoom());
    }

    const u32 want = clonesForParty();
    if (want == 0 && drops::encounterActive()) {
        // Still scale original HP even when no clones are needed.
        for (fpc_ProcID sourceId : g_notedSources) {
            if (fopAc_ac_c* source = fopAcM_SearchByID(sourceId)) {
                applySnapshottedHp(source);
            }
            if (!containsId(g_augmentedSources, sourceId)) {
                g_augmentedSources.push_back(sourceId);
            }
        }
        g_notedSources.clear();
        return;
    }
    if (want == 0) {
        g_notedSources.clear();
        return;
    }

    for (fpc_ProcID sourceId : g_notedSources) {
        if (containsId(g_augmentedSources, sourceId) || containsId(g_liveCloneIds, sourceId) ||
            containsId(g_pendingIds, sourceId)) {
            continue;
        }
        fopAc_ac_c* source = fopAcM_SearchByID(sourceId);
        if (source == nullptr) {
            continue;
        }
        applySnapshottedHp(source);
        for (u32 i = 0; i < want; ++i) {
            if (spawnClone(source, 0) == fpcM_ERROR_PROCESS_ID_e) {
                break;
            }
        }
        // One attempt per source per room — avoid placement-spam retries.
        if (!containsId(g_augmentedSources, sourceId)) {
            g_augmentedSources.push_back(sourceId);
        }
    }
    g_notedSources.clear();
}

int scanEligibleSources(void* actor, void* /*data*/) {
    auto* ac = static_cast<fopAc_ac_c*>(actor);
    if (ac == nullptr || fopAcM_GetGroup(ac) != fopAc_ENEMY_e) {
        return 0;
    }
    noteEligibleSource(ac);
    return 0;
}

#endif  // TARGET_PC

}  // namespace

void init() {
    g_adapters.clear();
    g_pendingWaves = 0;
#if TARGET_PC
    g_pendingIds.clear();
    g_liveCloneIds.clear();
    g_augmentedSources.clear();
    g_notedSources.clear();

    EnemyAdapter armos{};
    armos.procName = fpcNm_E_AI_e;
    armos.name = "E_AI";
    armos.whitelistClone = true;
    armos.maxClonesPerSource = 1;
    armos.maxClonesPerRoom = 4;
    armos.allowProgressionSwitch = false;
    armos.capsuleRadius = 80.0f;
    armos.capsuleHeight = 250.0f;
    armos.spawnOffset = 180.0f;
    registerAdapter(armos);
#endif
}

void reset() { init(); }

void tick() {
#if !TARGET_PC
    return;
#else
    resolvePending();
    pruneDeadClones();
    // Periodic room scan + Create-hook notes share the same queue.
    fopAcIt_Executor(scanEligibleSources, nullptr);
    processNotedSources();
#endif
}

bool registerAdapter(const EnemyAdapter& adapter) {
    if (adapter.procName < 0) {
        return false;
    }
    if (findAdapter(adapter.procName)) {
        return false;
    }
    g_adapters.push_back(adapter);
    return true;
}

const EnemyAdapter* findAdapter(s16 procName) {
    for (const auto& a : g_adapters) {
        if (a.procName == procName) {
            return &a;
        }
    }
    return nullptr;
}

void noteEligibleSource(fopAc_ac_c* source) {
#if !TARGET_PC
    (void)source;
    return;
#else
    if (source == nullptr) {
        return;
    }
    const fpc_ProcID id = fopAcM_GetID(source);
    if (id == fpcM_ERROR_PROCESS_ID_e || isClone(id)) {
        return;
    }
    // Stage originals carry a real set ID; clones / dynamic spawns use 0xFFFF.
    if (fopAcM_GetSetId(source) == 0xFFFF) {
        return;
    }
    const auto* adapter = findAdapter(actorProcName(source));
    if (adapter == nullptr || !adapter->whitelistClone) {
        return;
    }
    if (containsId(g_augmentedSources, id) || containsId(g_notedSources, id)) {
        return;
    }
    g_notedSources.push_back(id);
#endif
}

fpc_ProcID spawnClone(fopAc_ac_c* source, PlayerId /*reasonPlayer*/) {
#if !TARGET_PC
    (void)source;
    return fpcM_ERROR_PROCESS_ID_e;
#else
    if (source == nullptr) {
        return fpcM_ERROR_PROCESS_ID_e;
    }
    const fpc_ProcID sourceId = fopAcM_GetID(source);
    if (isClone(sourceId) || fopAcM_GetSetId(source) == 0xFFFF) {
        return fpcM_ERROR_PROCESS_ID_e;  // no recursive cloning
    }

    const auto* adapter = findAdapter(actorProcName(source));
    if (adapter == nullptr || !adapter->whitelistClone) {
        return fpcM_ERROR_PROCESS_ID_e;
    }
    if (g_liveCloneIds.size() + g_pendingIds.size() >= adapter->maxClonesPerRoom) {
        debug::logWarn("enemy: room clone cap (%u) reached", adapter->maxClonesPerRoom);
        return fpcM_ERROR_PROCESS_ID_e;
    }

    const u32 ordinal = static_cast<u32>(g_liveCloneIds.size() + g_pendingIds.size());
    cXyz pos;
    csXyz angle;
    if (!tryPlaceClone(source, *adapter, ordinal, &pos, &angle)) {
        debug::logWarn("enemy: no safe placement for clone of pid=%u",
                       static_cast<unsigned>(sourceId));
        return fpcM_ERROR_PROCESS_ID_e;
    }

    const u32 params =
        sanitizeParameters(adapter->procName, fopAcM_GetParam(source), *adapter);
    const s8 argument = source->argument;
    const cXyz* scale = fopAcM_GetScale_p(source);
    const int roomNo = fopAcM_GetRoomNo(source);

    // Same pattern as Gate D proxies: create on the play-scene layer.
    layer_class* savedLayer = fpcLy_CurrentLayer();
    base_process_class* playScene = fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
    if (playScene != nullptr) {
        fpcLy_SetCurrentLayer(&reinterpret_cast<process_node_class*>(playScene)->layer);
    }

    const fpc_ProcID pid =
        fopAcM_create(adapter->procName, 0xFFFF, params, &pos, roomNo, &angle, scale, argument,
                      nullptr);

    if (playScene != nullptr) {
        fpcLy_SetCurrentLayer(savedLayer);
    }

    if (pid == fpcM_ERROR_PROCESS_ID_e) {
        debug::logError("enemy: fopAcM_create failed for %s", adapter->name ? adapter->name : "?");
        return fpcM_ERROR_PROCESS_ID_e;
    }

    g_pendingIds.push_back(pid);
    debug::logInfo("enemy: clone requested pid=%u from=%u params=0x%08x setId=0xFFFF",
                   static_cast<unsigned>(pid), static_cast<unsigned>(sourceId), params);

    // Fast path: create already completed this frame.
    if (!fpcM_IsCreating(pid)) {
        if (fopAc_ac_c* actor = fopAcM_SearchByID(pid)) {
            eraseId(g_pendingIds, pid);
            if (!containsId(g_liveCloneIds, pid)) {
                g_liveCloneIds.push_back(pid);
            }
            applySnapshottedHp(actor);
        }
    }

    return pid;
#endif
}

bool isClone(fpc_ProcID id) {
#if !TARGET_PC
    (void)id;
    return false;
#else
    return containsId(g_liveCloneIds, id) || containsId(g_pendingIds, id);
#endif
}

bool isClone(fopAc_ac_c* actor) {
#if !TARGET_PC
    (void)actor;
    return false;
#else
    return actor != nullptr && isClone(fopAcM_GetID(actor));
#endif
}

u32 pendingCloneCount() {
#if !TARGET_PC
    return 0;
#else
    return static_cast<u32>(g_pendingIds.size());
#endif
}

u32 liveCloneCount() {
#if !TARGET_PC
    return 0;
#else
    return static_cast<u32>(g_liveCloneIds.size());
#endif
}

u32 pendingWaveCount() { return g_pendingWaves; }

bool roomClearBlocked() {
    // Live clones are already visible to fopAcM_myRoomSearchEnemy.
    // Block only on actors not yet in the layer, and on scheduled waves.
    return pendingCloneCount() > 0 || pendingWaveCount() > 0;
}

void onRoomUnload() {
    g_pendingWaves = 0;
#if TARGET_PC
    g_pendingIds.clear();
    g_liveCloneIds.clear();
    g_augmentedSources.clear();
    g_notedSources.clear();
    drops::endEncounter();
#endif
}

}  // namespace dusk::coop::enemy
