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

#if TARGET_PC
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

#if TARGET_PC

namespace {

constexpr f32 kSoftSepRadius = 45.0f;
constexpr f32 kSoftSepPush = 0.25f;
constexpr f32 kSoftSepAuthorityMultiplier = 2.0f;
constexpr f32 kSpawnOffset = 80.0f;

struct LinkMeta {
    fpc_ProcID processId = fpcM_ERROR_PROCESS_ID_e;
    bool pendingRecreate = false;
    bool createRequested = false;
};

std::array<LinkMeta, MAX_LOCAL_PLAYERS> g_meta{};

static bool isStoryAuthority(PlayerId id) {
    const auto* slot = playerSlot(id);
    return slot != nullptr && slot->transitionAuthority;
}

static PlayerId storyAuthorityId() {
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        if (isStoryAuthority(id)) {
            return id;
        }
    }
    return 0;
}

static bool isPlayerAlive(PlayerId id) {
    if (!isValidPlayer(id)) {
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

constexpr f32 kLinkEyeHeight = 150.0f;

static void syncAttention(fopAc_ac_c* actor) {
    if (actor == nullptr) {
        return;
    }
    actor->eyePos = actor->current.pos;
    actor->eyePos.y += kLinkEyeHeight;
    actor->attention_info.position = actor->eyePos;
}

static int authorityRoom() {
    fopAc_ac_c* authority = getPlayerActor(storyAuthorityId());
    if (authority == nullptr) {
        return 0;
    }
    return fopAcM_GetRoomNo(authority);
}

static bool tryPlaceNearAuthority(cXyz* outPos, s16* outYaw, PlayerId joiningId = 1) {
    fopAc_ac_c* authority = getPlayerActor(storyAuthorityId());
    if (authority == nullptr || outPos == nullptr || outYaw == nullptr) {
        return false;
    }

    // Eight distinct offsets so joining players do not pile onto one spawn point.
    static const cXyz kOffsets[] = {
        {kSpawnOffset, 0.0f, 0.0f},
        {-kSpawnOffset, 0.0f, 0.0f},
        {0.0f, 0.0f, kSpawnOffset},
        {0.0f, 0.0f, -kSpawnOffset},
        {kSpawnOffset, 0.0f, kSpawnOffset},
        {-kSpawnOffset, 0.0f, -kSpawnOffset},
        {kSpawnOffset, 0.0f, -kSpawnOffset},
        {-kSpawnOffset, 0.0f, kSpawnOffset},
    };
    constexpr size_t kOffsetCount = sizeof(kOffsets) / sizeof(kOffsets[0]);
    const size_t startIdx = static_cast<size_t>(joiningId) % kOffsetCount;

    const s16 yaw = authority->shape_angle.y;
    for (size_t n = 0; n < kOffsetCount; ++n) {
        const cXyz& local = kOffsets[(startIdx + n) % kOffsetCount];
        cXyz world = local;
        mDoMtx_stack_c::YrotS(yaw);
        mDoMtx_stack_c::multVec(&world, &world);
        world += authority->current.pos;
        world.y += 50.0f;

        if (!fopAcM_gc_c::gndCheck(&world)) {
            continue;
        }
        const f32 groundY = fopAcM_gc_c::getGroundY();
        if (groundY <= -G_CM3D_F_INF) {
            continue;
        }
        world.y = groundY;
        *outPos = world;
        *outYaw = yaw;
        return true;
    }

    *outPos = authority->current.pos;
    outPos->x += kSpawnOffset;
    *outYaw = yaw;
    return true;
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
    // NOTE: tiled multi-pass scissors currently black the Metal world path; 2P must
    // stay on dual-composite / same-camera (full-frame passes + L/R blit).
    if (span > 2) {
        render::setDualCameraCompositeEnabled(false);
        render::setSameCameraSplitEnabled(false);

        if (render::forcedViewCount() == span && runtime().activeViewCount == span) {
            return;
        }

        for (PlayerId i = 1; i < span; ++i) {
            if (!isJoined(i)) {
                continue;
            }
            if (getPlayerActor(i) == nullptr) {
                runtime().activeViewCount = 1;
                static bool sLoggedWait = false;
                if (!sLoggedWait) {
                    debug::logInfo(
                        "Co-op join: multi-view waiting for secondary Links before ensureCameras");
                    sLoggedWait = true;
                }
                return;
            }
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
    // Full-frame cam0 + cam1 renders, then L/R blit. Same-camera fallback until cam1
    // finishes dCamera init (field_0xb0c). Do not use setForcedViewCount — that path
    // blacks Metal's world draw while leaving the HUD visible.
    render::setForcedViewCount(0);
    render::setDualCameraCompositeEnabled(true);

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
                "Co-op join: waiting for secondary Link before ensureCameras — same-camera fallback");
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
        debug::logError("Co-op join: ensureCameras(%u) failed — same-camera split fallback", span);
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

    render::setSameCameraSplitEnabled(true);
    debug::logInfo(
        "Co-op join: dual-camera requested (same-camera present until cam1 init completes)");
}

static void resolvePendingCreates() {
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
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

static void recreatePendingLinks() {
    if (getPlayerActor(storyAuthorityId()) == nullptr) {
        return;
    }
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        if (isStoryAuthority(id)) {
            continue;
        }
        if (!g_meta[id].pendingRecreate || !isJoined(id)) {
            continue;
        }
        if (isPlayerAlive(id) || g_meta[id].createRequested) {
            g_meta[id].pendingRecreate = false;
            continue;
        }
        if (spawnPlayerLinkNearAuthority(id)) {
            g_meta[id].pendingRecreate = false;
        }
    }
}

#endif  // TARGET_PC

void init() {
#if TARGET_PC
    g_meta = {};
#endif
}

void reset() {
#if TARGET_PC
    destroyNonAuthorityLinks();
    g_meta = {};
#endif
}

void tick() {
#if !TARGET_PC
    return;
#else
    resolvePendingCreates();
    recreatePendingLinks();

    // Cameras wait for their tracked Link to exist in the native player slot.
    if (runtime().joinedPlayerCount >= 2) {
        syncCamerasForJoined();
    }

    // Keep overlapping player bodies apart without restricting how far they can travel.
    const PlayerId authority = storyAuthorityId();
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        if (id == authority) {
            continue;
        }
        if (!isJoined(id) || !isPlayerAlive(id)) {
            continue;
        }
        softSeparate(authority, id);
        for (PlayerId other = id + 1; other < MAX_LOCAL_PLAYERS; ++other) {
            if (isJoined(other) && isPlayerAlive(other)) {
                softSeparate(id, other);
            }
        }
    }
#endif
}

bool spawnPlayerLink(PlayerId id, const cXyz& pos, s16 yaw) {
#if !TARGET_PC
    (void)id;
    (void)pos;
    (void)yaw;
    return false;
#else
    if (!isValidPlayer(id) || isStoryAuthority(id)) {
        return false;
    }
    if (isPlayerAlive(id) || g_meta[id].createRequested) {
        return true;
    }

    // Register the slot before creating the actor so init can bind it to native player storage.
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

    // Phase 6+: real daAlink_c per co-op player (up to MAX_LOCAL_PLAYERS).
    alink::registerPendingSpawn(id);
    const fpc_ProcID pid =
        fopAcM_create(fpcNm_ALINK_e, 0xFFFF, 0, &pos, roomNo, &angle, nullptr, -1, nullptr);

    if (playScene != nullptr) {
        fpcLy_SetCurrentLayer(savedLayer);
    }

    if (pid == fpcM_ERROR_PROCESS_ID_e) {
        alink::clearSpawn(id);
        debug::logError("spawnPlayerLink: fopAcM_create(ALINK) failed for P%u", id);
        return false;
    }

    alink::noteSpawnProcess(id, static_cast<u32>(pid));
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
    debug::logInfo("Link P%u spawn requested (pid=%u)", id, static_cast<unsigned>(pid));
    return true;
#endif
}

bool spawnPlayerLinkNearAuthority(PlayerId id) {
#if !TARGET_PC
    (void)id;
    return false;
#else
    cXyz pos;
    s16 yaw = 0;
    if (!tryPlaceNearAuthority(&pos, &yaw, id)) {
        debug::logWarn("spawnPlayerLinkNearAuthority: no authority player yet for P%u", id);
        if (auto* slot = playerSlot(id)) {
            slot->joined = true;
            slot->enabled = true;
            slot->id = id;
        }
        g_meta[id].pendingRecreate = true;
        return false;
    }
    return spawnPlayerLink(id, pos, yaw);
#endif
}

void destroyPlayerLink(PlayerId id) {
#if !TARGET_PC
    (void)id;
    return;
#else
    if (!isValidPlayer(id) || isStoryAuthority(id)) {
        return;
    }

    const fpc_ProcID pid = g_meta[id].processId;
    fopAc_ac_c* actor = getPlayerActor(id);
    if (actor == nullptr && pid != fpcM_ERROR_PROCESS_ID_e) {
        actor = fopAcM_SearchByID(pid);
    }

    if (actor != nullptr) {
        // Leave the native slot registered until the Alink destructor resolves its owner.
        fopAcM_delete(actor);
    } else {
        setPlayerActor(id, nullptr);
        combat::unregisterPlayerActor(id);
    }

    if (auto* slot = playerSlot(id)) {
        slot->joined = false;
        slot->enabled = false;
        slot->view.reset();
    }
    clearMeta(id, /*keepPending=*/false);
    alink::clearSpawn(id);
#endif
}

void destroyNonAuthorityLinks() {
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        if (!isStoryAuthority(id)) {
            destroyPlayerLink(id);
        }
    }
}

bool softSeparate(PlayerId a, PlayerId b) {
#if !TARGET_PC
    (void)a;
    (void)b;
    return false;
#else
    if (a == b || !isValidPlayer(a) || !isValidPlayer(b)) {
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
    // Story authority is immovable; other players take the full push.
    if (isStoryAuthority(a)) {
        actorB->current.pos += delta * (push * kSoftSepAuthorityMultiplier);
        syncAttention(actorB);
    } else if (isStoryAuthority(b)) {
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

void onRoomUnload() {
#if !TARGET_PC
    return;
#else
    // Stage teardown owns the story-authority actor; recreate every other joined actor.
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        if (isStoryAuthority(id)) {
            continue;
        }
        if (!isJoined(id) && !g_meta[id].pendingRecreate) {
            continue;
        }

        const fpc_ProcID pid = g_meta[id].processId;
        fopAc_ac_c* actor = getPlayerActor(id);
        if (actor == nullptr && pid != fpcM_ERROR_PROCESS_ID_e) {
            actor = fopAcM_SearchByID(pid);
        }
        if (actor != nullptr) {
            // Keep the native slot through fopAcM_delete so Alink can resolve its owner.
            fopAcM_delete(actor);
        }
        if (auto* slot = playerSlot(id)) {
            if (getPlayerActor(id) == actor) {
                setPlayerActor(id, nullptr);
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
#if !TARGET_PC
    (void)id;
    return false;
#else
    if (!isValidPlayer(id) || isStoryAuthority(id)) {
        return false;
    }
    const bool spawned = spawnPlayerLinkNearAuthority(id);
    syncCamerasForJoined();
    return spawned || g_meta[id].pendingRecreate;
#endif
}

void onLinkReady(PlayerId id) {
#if !TARGET_PC
    (void)id;
    return;
#else
    if (!isValidPlayer(id)) {
        return;
    }
    g_meta[id].createRequested = false;
    g_meta[id].pendingRecreate = false;
    if (fopAc_ac_c* actor = getPlayerActor(id)) {
        g_meta[id].processId = fopAcM_GetID(actor);
    }
    syncCamerasForJoined();
    debug::logInfo("Link P%u ready", id);
#endif
}

}  // namespace dusk::coop::player
