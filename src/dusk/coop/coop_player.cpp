#include "dusk/coop/coop_player.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_accessors.h"
#include "dusk/coop/coop_alink.h"
#include "dusk/coop/coop_camera.h"
#include "dusk/coop/coop_combat.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_forms.h"
#include "dusk/coop/coop_horses.h"
#include "dusk/coop/coop_input.h"
#include "dusk/coop/coop_render.h"

#include <array>
#include <cmath>
#include <cstring>

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_camera_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "f_pc/f_pc_node.h"
#include "m_Do/m_Do_mtx.h"
#endif

namespace dusk::coop::player {

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC

namespace {

constexpr f32 kSoftSepRadius = 45.0f;
constexpr f32 kSoftSepPush = 0.25f;
constexpr f32 kSoftSepAuthorityMultiplier = 2.0f;
constexpr f32 kTetherWarnDist = 800.0f;
constexpr f32 kTetherTeleportDist = 1500.0f;
constexpr f32 kSpawnOffset = 80.0f;

struct ProxyMeta {
    fpc_ProcID processId = fpcM_ERROR_PROCESS_ID_e;
    bool pendingRecreate = false;
    bool createRequested = false;
};

std::array<ProxyMeta, MAX_LOCAL_PLAYERS> g_meta{};

static bool isPlayerAlive(PlayerId id) {
    if (!isValidPlayer(id) || id == 0) {
        return false;
    }
    fopAc_ac_c* actor = getPlayerActor(id);
    if (actor == nullptr) {
        return false;
    }
    if (g_meta[id].processId != fpcM_ERROR_PROCESS_ID_e) {
        fopAc_ac_c* byId = fopAcM_SearchByID(g_meta[id].processId);
        return byId == actor;
    }
    return fopAcM_GetName(actor) == fpcNm_ALINK_e;
}

static void clearMeta(PlayerId id, bool keepPending) {
    if (!isValidPlayer(id)) {
        return;
    }
    const bool pending = keepPending && g_meta[id].pendingRecreate;
    g_meta[id] = {};
    g_meta[id].pendingRecreate = pending;
}

}  // namespace

static uint8_t computeJoinedViewSpan() {
    uint8_t span = 1;
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (isJoined(i)) {
            span = static_cast<uint8_t>(i + 1);
        }
    }
    return span;
}

static void syncCamerasForJoined() {
    const uint8_t span = computeJoinedViewSpan();
    if (span < 2) {
        return;
    }

    render::setIncompatibleEffectsDisabled(true);

    // ── Multi-view tiled mode for 3+ players ──
    if (span > 2) {
        // Already set up — nothing to do each frame.
        if (render::forcedViewCount() == span && runtime().activeViewCount == span) {
            return;
        }

        bool secondaryPlayerReady = false;
        for (PlayerId i = 1; i < span; ++i) {
            if (isJoined(i) && getPlayerActor(i) != nullptr) {
                secondaryPlayerReady = true;
                break;
            }
        }
        if (!secondaryPlayerReady) {
            runtime().activeViewCount = 1;
            static bool sLoggedWait = false;
            if (!sLoggedWait) {
                debug::logInfo(
                    "Co-op join: multi-view waiting for proxy actors before ensureCameras");
                sLoggedWait = true;
            }
            return;
        }

        if (!camera::ensureCameras(span)) {
            debug::logError("Co-op join: ensureCameras(%u) failed for multi-view", span);
            runtime().activeViewCount = 1;
            for (PlayerId i = 1; i < span; ++i) {
                if (auto* slot = playerSlot(i)) {
                    slot->view = 0;
                }
            }
            return;
        }

        for (PlayerId i = 1; i < span; ++i) {
            if (!isJoined(i)) {
                continue;
            }
            camera::assignTrackedPlayer(i, i);
            camera::assignInputOwner(i, i);
            camera::assignAttentionOwner(i, i);
            if (auto* slot = playerSlot(i)) {
                slot->view = i;
            }
        }

        runtime().activeViewCount = span;
        render::setForcedViewCount(span);
        debug::logInfo("Co-op join: multi-view tiled mode with %u cameras", span);
        return;
    }

    // ── 2-player dual-camera composite path ──
    // Docs (Task 02/03, Gate A→B): sim once; cameras via scheduler; painter binds
    // matrices. ISSUE: calling ensureCameras before the proxy is in the player sidecar
    // leaves cam1 stuck in init_phase2; creating too early also hit Z2 audio OOB on
    // first draw. Same-camera L/R blit until dualCameraCompositeReady().
    render::setForcedViewCount(0);
    render::setDualCameraCompositeEnabled(true);

    // Already past Gate B handoff — nothing to do.
    if (render::dualCameraCompositeReady()) {
        return;
    }

    bool secondaryPlayerReady = false;
    for (PlayerId i = 1; i < span; ++i) {
        if (isJoined(i) && getPlayerActor(i) != nullptr) {
            secondaryPlayerReady = true;
            break;
        }
    }
    if (!secondaryPlayerReady) {
        render::setSameCameraSplitEnabled(true);
        runtime().activeViewCount = 1;
        static bool sLoggedWait = false;
        if (!sLoggedWait) {
            debug::logInfo(
                "Co-op join: waiting for proxy actor before ensureCameras — same-camera fallback");
            sLoggedWait = true;
        }
        return;
    }

    // Camera process may exist but still be in init_phase2 — keep fallback present.
    if (camera::isCameraActive(1) && getCameraProcess(1) != nullptr) {
        render::setSameCameraSplitEnabled(true);
        return;
    }

    if (!camera::ensureCameras(span)) {
        debug::logError("Co-op join: ensureCameras(%u) failed — same-camera split fallback",
                        span);
        render::setSameCameraSplitEnabled(true);
        runtime().activeViewCount = 1;
        for (PlayerId i = 1; i < span; ++i) {
            if (auto* slot = playerSlot(i)) {
                slot->view = 0;
            }
        }
        return;
    }

    for (PlayerId i = 1; i < span; ++i) {
        if (!isJoined(i)) {
            continue;
        }
        camera::assignTrackedPlayer(i, i);
        camera::assignInputOwner(i, i);
        camera::assignAttentionOwner(i, i);
        if (auto* slot = playerSlot(i)) {
            slot->view = i;
        }
    }

    // Keep same-camera until cam1's dCamera body is constructed (see camerasReadyForDual).
    render::setSameCameraSplitEnabled(true);
    debug::logInfo(
        "Co-op join: dual-camera requested (same-camera present until cam1 init completes)");
}

static void resolvePendingCreates() {
    for (PlayerId id = 1; id < MAX_LOCAL_PLAYERS; ++id) {
        auto& meta = g_meta[id];
        if (!meta.createRequested || meta.processId == fpcM_ERROR_PROCESS_ID_e) {
            continue;
        }
        fopAc_ac_c* actor = fopAcM_SearchByID(meta.processId);
        if (actor == nullptr) {
            continue;
        }
        setPlayerActor(id, actor);
        combat::registerPlayerActor(id, actor);
        meta.createRequested = false;
    }
}

static void recreatePendingProxies() {
    if (getPlayerActor(0) == nullptr) {
        return;
    }
    for (PlayerId id = 1; id < MAX_LOCAL_PLAYERS; ++id) {
        if (!g_meta[id].pendingRecreate || !isJoined(id)) {
            continue;
        }
        if (isPlayerAlive(id) || g_meta[id].createRequested) {
            g_meta[id].pendingRecreate = false;
            continue;
        }
        if (spawnSecondaryLinkNearAuthority(id)) {
            g_meta[id].pendingRecreate = false;
        }
    }
}

#endif  // ENABLE_LOCAL_COOP && TARGET_PC

}  // namespace

void init() {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    g_meta = {};
#endif
}

void reset() {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    destroyAllSecondaryLinks();
    g_meta = {};
#endif
}

void tick() {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    return;
#else
    if (!isEnabled()) {
        return;
    }

    resolvePendingCreates();
    recreatePendingProxies();

    // Cameras need the proxy in the player sidecar (init_phase2). Retry wiring after resolve.
    if (runtime().joinedPlayerCount >= 2) {
        syncCamerasForJoined();
    }

    // Soft separation + tether after proxy execute (same frame is fine; next frame settles).
    for (PlayerId id = 1; id < MAX_LOCAL_PLAYERS; ++id) {
        if (!isJoined(id) || !isPlayerAlive(id)) {
            continue;
        }
        softSeparate(0, id);
        tetherTeleportIfNeeded(id, 0);
        for (PlayerId other = id + 1; other < MAX_LOCAL_PLAYERS; ++other) {
            if (isJoined(other) && isPlayerAlive(other)) {
                softSeparate(id, other);
            }
        }
    }
#endif
}

bool spawnSecondaryLink(PlayerId id, const cXyz& pos, s16 yaw) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)id;
    (void)pos;
    (void)yaw;
    return false;
#else
    if (!isEnabled() || id == 0 || !isValidPlayer(id)) {
        return false;
    }
    if (isPlayerAlive(id) || g_meta[id].createRequested) {
        return true;
    }

    // Never write secondary players into the original one-slot player array.
    if (auto* slot = playerSlot(id)) {
        slot->joined = true;
        slot->enabled = true;
        slot->id = id;
        if (!slot->view.has_value()) {
            slot->view = static_cast<ViewId>(id);
        }
    }

    csXyz angle(0, yaw, 0);
    const int roomNo = authorityRoom();

    // Layer must be the play scene so the actor survives room actor sweeps correctly.
    layer_class* savedLayer = fpcLy_CurrentLayer();
    base_process_class* playScene = fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
    if (playScene != nullptr) {
        fpcLy_SetCurrentLayer(&reinterpret_cast<process_node_class*>(playScene)->layer);
    }

    // Phase 6: real daAlink_c. Ownership via pending registry (not Link params).
    alink::registerPendingSpawn(id);
    const fpc_ProcID pid =
        fopAcM_create(fpcNm_ALINK_e, 0xFFFF, 0, &pos, roomNo, &angle, nullptr, -1, nullptr);

    if (playScene != nullptr) {
        fpcLy_SetCurrentLayer(savedLayer);
    }

    if (pid == fpcM_ERROR_PROCESS_ID_e) {
        alink::clearPendingSpawn();
        debug::logError("spawnSecondaryLink: fopAcM_create(ALINK) failed for P%u", id);
        return false;
    }

    g_meta[id].processId = pid;
    g_meta[id].createRequested = true;
    g_meta[id].pendingRecreate = false;

    // Resolve immediately when the scheduler already completed create.
    if (fopAc_ac_c* actor = fopAcM_SearchByID(pid)) {
        if (fopAcM_GetName(actor) == fpcNm_ALINK_e) {
            setPlayerActor(id, actor);
            g_meta[id].createRequested = false;
            combat::registerPlayerActor(id, actor);
        }
    }

    syncCamerasForJoined();
    debug::logInfo("Secondary Link P%u spawn requested (pid=%u)", id, static_cast<unsigned>(pid));
    return true;
#endif
}

bool spawnSecondaryLinkNearAuthority(PlayerId id) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)id;
    return false;
#else
    cXyz pos;
    s16 yaw = 0;
    if (!tryPlaceNearAuthority(&pos, &yaw)) {
        debug::logWarn("spawnSecondaryLinkNearAuthority: no authority player yet for P%u", id);
        if (auto* slot = playerSlot(id)) {
            slot->joined = true;
            slot->enabled = true;
            slot->id = id;
        }
        g_meta[id].pendingRecreate = true;
        return false;
    }
    return spawnSecondaryLink(id, pos, yaw);
#endif
}

void destroySecondaryLink(PlayerId id) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)id;
    return;
#else
    if (id == 0 || !isValidPlayer(id)) {
        return;
    }

    const fpc_ProcID pid = g_meta[id].processId;
    fopAc_ac_c* actor = getPlayerActor(id);
    if (actor == nullptr && pid != fpcM_ERROR_PROCESS_ID_e) {
        actor = fopAcM_SearchByID(pid);
    }

    if (actor != nullptr) {
        // Leave sidecar until Alink destructor clears it (needs owner lookup).
        fopAcM_delete(actor);
    } else {
        setPlayerActor(id, nullptr);
        combat::unregisterPlayerActor(id);
    }

    if (auto* slot = playerSlot(id)) {
        slot->actor = nullptr;
        slot->joined = false;
        slot->enabled = false;
        slot->view.reset();
    }
    clearMeta(id, /*keepPending=*/false);
    if (alink::peekPendingOwner() == id) {
        alink::clearPendingSpawn();
    }
#endif
}

void destroyAllSecondaryLinks() {
    for (PlayerId i = 1; i < MAX_LOCAL_PLAYERS; ++i) {
        destroySecondaryLink(i);
    }
}

bool softSeparate(PlayerId a, PlayerId b) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)a;
    (void)b;
    return false;
#else
    if (!isEnabled() || a == b || !isValidPlayer(a) || !isValidPlayer(b)) {
        return false;
    }
    fopAc_ac_c* actorA = getPlayerActor(a);
    fopAc_ac_c* actorB = getPlayerActor(b);
    if (actorA == nullptr || actorB == nullptr) {
        return false;
    }

    cXyz delta = actorB->current.pos - actorA->current.pos;
    delta.y = 0.0f;
    const f32 distance = delta.abs();
    if (distance <= 0.001f || distance >= kSoftSepRadius) {
        return false;
    }

    if (!delta.normalizeRS()) {
        return false;
    }
    const f32 push = (kSoftSepRadius - distance) * kSoftSepPush;
    // Authority (player 0) is immovable; secondary proxies take the full push.
    if (a == 0) {
        actorB->current.pos += delta * (push * kSoftSepAuthorityMultiplier);
        syncAttention(actorB);
    } else if (b == 0) {
        actorA->current.pos -= delta * (push * kSoftSepAuthorityMultiplier);
        syncAttention(actorA);
    } else {
        actorA->current.pos -= delta * push;
        actorB->current.pos += delta * push;
        syncAttention(actorA);
        syncAttention(actorB);
    }
    return true;
#endif
}

bool tetherTeleportIfNeeded(PlayerId follower, PlayerId authority) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)follower;
    (void)authority;
    return false;
#else
    if (!isEnabled() || follower == 0 || authority != 0) {
        return false;
    }
    fopAc_ac_c* proxy = getPlayerActor(follower);
    fopAc_ac_c* auth = getPlayerActor(authority);
    if (proxy == nullptr || auth == nullptr) {
        return false;
    }

    cXyz delta = proxy->current.pos - auth->current.pos;
    const f32 distance = delta.abs();
    const bool roomMismatch = fopAcM_GetRoomNo(proxy) != fopAcM_GetRoomNo(auth);

    if (distance > kTetherWarnDist && distance <= kTetherTeleportDist && !roomMismatch) {
        // Soft pull toward authority.
        delta.y = 0.0f;
        if (delta.normalizeRS()) {
            proxy->current.pos -= delta * 8.0f;
            syncAttention(proxy);
        }
        return false;
    }

    if (distance <= kTetherTeleportDist && !roomMismatch) {
        return false;
    }

    cXyz dest;
    s16 yaw = 0;
    if (!tryPlaceNearAuthority(&dest, &yaw)) {
        return false;
    }
    proxy->current.pos = dest;
    proxy->old.pos = dest;
    proxy->home.pos = dest;
    proxy->shape_angle.y = yaw;
    proxy->current.angle.y = yaw;
    proxy->speed.setall(0.0f);
    proxy->speedF = 0.0f;
    syncAttention(proxy);
    debug::logInfo("Proxy P%u tether-teleported (dist=%.1f roomMismatch=%d)", follower, distance,
                   roomMismatch ? 1 : 0);
    return true;
#endif
}

void onRoomUnload() {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    return;
#else
    // Destroy bodies but remember which joined players need a recreate after the next room loads.
    for (PlayerId id = 1; id < MAX_LOCAL_PLAYERS; ++id) {
        const bool wasJoined = isJoined(id) || isPlayerAlive(id) || g_meta[id].pendingRecreate;
        if (!wasJoined) {
            continue;
        }

        const fpc_ProcID pid = g_meta[id].processId;
        fopAc_ac_c* actor = getPlayerActor(id);
        if (actor == nullptr && pid != fpcM_ERROR_PROCESS_ID_e) {
            actor = fopAcM_SearchByID(pid);
        }
        if (actor != nullptr) {
            // Keep sidecar through fopAcM_delete so Alink destructor can resolve owner.
            fopAcM_delete(actor);
        }
        if (auto* slot = playerSlot(id)) {
            // Destructor clears actor; force-clear if delete was a no-op.
            if (slot->actor == actor) {
                slot->actor = nullptr;
            }
            // Keep joined so Press-Start slot + input identity survive the transition.
            slot->joined = true;
            slot->enabled = true;
        }
        g_meta[id].processId = fpcM_ERROR_PROCESS_ID_e;
        g_meta[id].createRequested = false;
        g_meta[id].pendingRecreate = true;
    }
#endif
}

bool onPlayerJoined(PlayerId id) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)id;
    return false;
#else
    if (!isValidPlayer(id) || id == 0) {
        return false;
    }
    if (!isEnabled()) {
        setEnabled(true);
    }
    const bool spawned = spawnSecondaryLinkNearAuthority(id);
    syncCamerasForJoined();
    return spawned || g_meta[id].pendingRecreate;
#endif
}

void onSecondaryLinkReady(PlayerId id) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)id;
    return;
#else
    if (!isValidPlayer(id) || id == 0) {
        return;
    }
    g_meta[id].createRequested = false;
    g_meta[id].pendingRecreate = false;
    if (fopAc_ac_c* actor = getPlayerActor(id)) {
        g_meta[id].processId = fopAcM_GetID(actor);
    }
    syncCamerasForJoined();
    debug::logInfo("Secondary Link P%u ready", id);
#endif
}

}  // namespace dusk::coop::player
