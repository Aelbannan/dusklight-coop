#include "dusk/coop/coop_horses.h"
#include "dusk/coop/coop_horse_bridge.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_accessors.h"
#include "dusk/coop/coop_context.h"
#include "dusk/coop/coop_debug.h"

#include <array>
#include <cstring>

#if TARGET_PC
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_horse.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "f_pc/f_pc_node.h"
#include "m_Do/m_Do_mtx.h"
#endif

namespace dusk::coop::horses {

namespace {

#if TARGET_PC

constexpr PlayerId kNoPending = 0xFF;
constexpr f32 kSpawnOffset = 120.0f;

struct HorseMeta {
    fpc_ProcID processId = fpcM_ERROR_PROCESS_ID_e;
    bool createRequested = false;
    bool pendingRecreate = false;
    cXyz spawnPos{};
    s16 spawnYaw = 0;
};

std::array<HorseMeta, MAX_LOCAL_PLAYERS> g_meta{};
PlayerId g_pendingCreateOwner = kNoPending;

static PlayerId storyAuthorityId() {
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        if (const auto* player = playerSlot(id);
            player != nullptr && player->transitionAuthority) {
            return id;
        }
    }
    return 0;
}

static void clearMeta(PlayerId id, bool keepPending) {
    if (!isValidPlayer(id)) {
        return;
    }
    const bool pending = keepPending && g_meta[id].pendingRecreate;
    const cXyz pos = g_meta[id].spawnPos;
    const s16 yaw = g_meta[id].spawnYaw;
    g_meta[id] = {};
    if (pending) {
        g_meta[id].pendingRecreate = true;
        g_meta[id].spawnPos = pos;
        g_meta[id].spawnYaw = yaw;
    }
}

static bool isHorseAlive(PlayerId id) {
    daHorse_c* horse = getOwnedHorse(id);
    if (horse == nullptr) {
        return false;
    }
    if (g_meta[id].processId != fpcM_ERROR_PROCESS_ID_e) {
        return fopAcM_SearchByID(g_meta[id].processId) == reinterpret_cast<fopAc_ac_c*>(horse);
    }
    return fopAcM_GetName(reinterpret_cast<fopAc_ac_c*>(horse)) == fpcNm_HORSE_e;
}

static int authorityRoom() {
    fopAc_ac_c* authority = getPlayerActor(storyAuthorityId());
    if (authority == nullptr) {
        return dComIfGp_roomControl_getStayNo();
    }
    return fopAcM_GetRoomNo(authority);
}

static bool placeNearPlayer(PlayerId id, cXyz* outPos, s16* outYaw) {
    fopAc_ac_c* player = getPlayerActor(id);
    if (player == nullptr) {
        player = getPlayerActor(storyAuthorityId());
    }
    if (player == nullptr || outPos == nullptr || outYaw == nullptr) {
        return false;
    }

    static const cXyz kOffsets[] = {
        {kSpawnOffset, 0.0f, 0.0f},
        {-kSpawnOffset, 0.0f, 0.0f},
        {0.0f, 0.0f, kSpawnOffset},
        {0.0f, 0.0f, -kSpawnOffset},
    };

    const s16 yaw = player->shape_angle.y;
    for (const cXyz& local : kOffsets) {
        cXyz world = local;
        mDoMtx_stack_c::YrotS(yaw);
        mDoMtx_stack_c::multVec(&world, &world);
        world += player->current.pos;
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

    *outPos = player->current.pos;
    outPos->x += kSpawnOffset;
    *outYaw = yaw;
    return true;
}

static void resolvePendingCreates() {
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        if (!g_meta[id].createRequested) {
            continue;
        }
        if (g_meta[id].processId == fpcM_ERROR_PROCESS_ID_e) {
            continue;
        }
        fopAc_ac_c* actor = fopAcM_SearchByID(g_meta[id].processId);
        if (actor == nullptr) {
            continue;
        }
        if (fopAcM_GetName(actor) != fpcNm_HORSE_e) {
            continue;
        }
        // create() registers via onCreateSuccess; just clear the request flag.
        g_meta[id].createRequested = false;
        if (getOwnedHorse(id) == nullptr) {
            onCreateSuccess(id, reinterpret_cast<daHorse_c*>(actor), g_meta[id].processId);
        }
    }
}

static void recreatePendingHorses() {
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        if (!g_meta[id].pendingRecreate || !isJoined(id)) {
            continue;
        }
        if (isHorseAlive(id) || g_meta[id].createRequested) {
            g_meta[id].pendingRecreate = false;
            continue;
        }
        if (spawnOwnedHorse(id, g_meta[id].spawnPos, g_meta[id].spawnYaw)) {
            g_meta[id].pendingRecreate = false;
        }
    }
}

#endif  // TARGET_PC

}  // namespace

void init() {
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        runtime().horses[i] = {};
        runtime().horses[i].owner = i;
    }
#if TARGET_PC
    g_meta = {};
    g_pendingCreateOwner = kNoPending;
#endif
}

void reset() { init(); }

void tick() {
#if !TARGET_PC
    return;
#else
    resolvePendingCreates();
    recreatePendingHorses();
#endif
}

void onRoomUnload() {
#if !TARGET_PC
    return;
#else
    // Despawn indexed horses on room unload; keep ownership / last pose for recreate.
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        auto& s = slot(id);
        daHorse_c* horse = getOwnedHorse(id);
        if (!s.summoned && horse == nullptr) {
            continue;
        }
        if (horse != nullptr) {
            g_meta[id].spawnPos = horse->current.pos;
            g_meta[id].spawnYaw = horse->shape_angle.y;
            s.lastKnownPosition = horse->current.pos;
            s.lastKnownYaw = horse->shape_angle.y;
        }
        const bool wasSummoned = s.summoned;
        destroyOwnedHorse(id);
        if (wasSummoned && isJoined(id)) {
            g_meta[id].pendingRecreate = true;
            s.summoned = true;
        }
    }
#endif
}

HorseSlot& slot(PlayerId id) {
    COOP_ASSERT(isValidPlayer(id));
    return runtime().horses[id];
}

bool spawnOwnedHorse(PlayerId id, const cXyz& pos, s16 yaw) {
#if !TARGET_PC
    (void)id;
    (void)pos;
    (void)yaw;
    return false;
#else
    if (!isValidPlayer(id)) {
        return false;
    }

    if (isHorseAlive(id) || g_meta[id].createRequested) {
        return true;
    }

    auto& s = slot(id);
    s.owner = id;
    s.summoned = true;
    s.lastKnownPosition = pos;
    s.lastKnownYaw = yaw;
    g_meta[id].spawnPos = pos;
    g_meta[id].spawnYaw = yaw;

    csXyz angle(0, yaw, 0);
    const int roomNo = authorityRoom();

    // Path 0xFF = none; room-flag nibble = 1 so create uses actor room.
    constexpr u32 kCoopHorseParam = 0x000001FFu;

    layer_class* savedLayer = fpcLy_CurrentLayer();
    base_process_class* playScene = fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
    if (playScene != nullptr) {
        fpcLy_SetCurrentLayer(&reinterpret_cast<process_node_class*>(playScene)->layer);
    }

    beginPendingCreate(id);
    const fpc_ProcID pid =
        fopAcM_create(fpcNm_HORSE_e, 0xFFFF, kCoopHorseParam, &pos, roomNo, &angle, nullptr, -1,
                      nullptr);

    if (playScene != nullptr) {
        fpcLy_SetCurrentLayer(savedLayer);
    }

    if (pid == fpcM_ERROR_PROCESS_ID_e) {
        onCreateFailed(id);
        debug::logError("spawnOwnedHorse: fopAcM_create failed for P%u", id);
        return false;
    }

    g_meta[id].processId = pid;
    g_meta[id].createRequested = true;
    g_meta[id].pendingRecreate = false;

    if (fopAc_ac_c* actor = fopAcM_SearchByID(pid)) {
        if (getOwnedHorse(id) == nullptr && fopAcM_GetName(actor) == fpcNm_HORSE_e) {
            // create may still be mid-phase; registration happens in onCreateSuccess.
        }
        g_meta[id].createRequested = getOwnedHorse(id) == nullptr;
    }

    debug::logInfo("Horse P%u spawn requested (pid=%u) at (%.1f, %.1f, %.1f)", id,
                   static_cast<unsigned>(pid), pos.x, pos.y, pos.z);
    return true;
#endif
}

bool spawnOwnedHorseNearPlayer(PlayerId id) {
#if !TARGET_PC
    (void)id;
    return false;
#else
    cXyz pos;
    s16 yaw = 0;
    if (!placeNearPlayer(id, &pos, &yaw)) {
        debug::logWarn("spawnOwnedHorseNearPlayer: no player position for P%u", id);
        if (isValidPlayer(id)) {
            g_meta[id].pendingRecreate = true;
            slot(id).summoned = true;
            slot(id).owner = id;
        }
        return false;
    }
    return spawnOwnedHorse(id, pos, yaw);
#endif
}

void destroyOwnedHorse(PlayerId id) {
    if (!isValidPlayer(id)) {
        return;
    }
    auto& s = slot(id);

#if TARGET_PC
    if (daHorse_c* horse = getOwnedHorse(id)) {
        fopAc_ac_c* actor = reinterpret_cast<fopAc_ac_c*>(horse);
        // Destructor clears indexed ownership; null first to avoid re-entry.
        dComIfGp_setHorseActor(id, nullptr);
        fopAcM_delete(actor);
    }
    clearMeta(id, /*keepPending=*/false);
#else
    setOwnedHorse(id, nullptr);
#endif

    s.summoned = false;
    s.mounted = false;
}

void destroyAllHorses() {
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        destroyOwnedHorse(i);
    }
}

daHorse_c* resolveForContext() {
#if TARGET_PC
    const PlayerId id = currentPlayer();
    if (daHorse_c* mounted = resolveMounted(id)) {
        return mounted;
    }
    return resolveOwned(id);
#else
    return getOwnedHorse(currentPlayer());
#endif
}

daHorse_c* resolveOwned(PlayerId id) {
    return getOwnedHorse(id);
}

daHorse_c* resolveMounted(PlayerId id) {
    if (!isValidPlayer(id)) {
        return nullptr;
    }

#if TARGET_PC
    fopAc_ac_c* player = getPlayerActor(id);
    if (player != nullptr && fopAcM_GetName(player) == fpcNm_ALINK_e) {
        auto* link = static_cast<daAlink_c*>(player);
        if (link->checkHorseRide()) {
            if (fopAc_ac_c* ride = link->getRideActor()) {
                if (fopAcM_GetName(ride) == fpcNm_HORSE_e) {
                    return static_cast<daHorse_c*>(ride);
                }
            }
        }
    }
#endif

    auto& s = slot(id);
    return s.mounted ? getOwnedHorse(id) : nullptr;
}

daHorse_c* horseForPlayer(PlayerId id) { return resolveOwned(id); }

daHorse_c* mountedHorseForLink(const daAlink_c* link) {
#if !TARGET_PC
    (void)link;
    return nullptr;
#else
    if (link == nullptr || !link->checkHorseRide()) {
        return nullptr;
    }
    fopAc_ac_c* ride = const_cast<daAlink_c*>(link)->getRideActor();
    if (ride == nullptr || fopAcM_GetName(ride) != fpcNm_HORSE_e) {
        return nullptr;
    }
    return static_cast<daHorse_c*>(ride);
#endif
}

bool setMounted(PlayerId id, bool mounted) {
    if (!isValidPlayer(id)) {
        return false;
    }
    slot(id).mounted = mounted;
    return true;
}

u8 liveHorseCount() {
    u8 n = 0;
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (getOwnedHorse(i) != nullptr || slot(i).summoned) {
            ++n;
        }
    }
    return n;
}

PlayerId ownerOf(const daHorse_c* horse) {
    if (horse == nullptr) {
        return 0;
    }
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (getOwnedHorse(i) == horse) {
            return i;
        }
    }
    return 0;
}

void beginPendingCreate(PlayerId id) {
#if TARGET_PC
    g_pendingCreateOwner = isValidPlayer(id) ? id : kNoPending;
#else
    (void)id;
#endif
}

PlayerId peekPendingCreateOwner() {
#if TARGET_PC
    if (g_pendingCreateOwner != kNoPending) {
        return g_pendingCreateOwner;
    }
#endif
    return 0;
}

bool hasPendingCreate() {
#if TARGET_PC
    return g_pendingCreateOwner != kNoPending;
#else
    return false;
#endif
}

void onCreateSuccess(PlayerId id, daHorse_c* horse, fpc_ProcID pid) {
    if (!isValidPlayer(id) || horse == nullptr) {
        return;
    }
    setOwnedHorse(id, horse);
    auto& s = slot(id);
    s.owner = id;
    s.summoned = true;
    s.lastKnownPosition = horse->current.pos;
    s.lastKnownYaw = horse->shape_angle.y;

#if TARGET_PC
    g_meta[id].processId = pid;
    g_meta[id].createRequested = false;
    g_meta[id].pendingRecreate = false;
    if (g_pendingCreateOwner == id) {
        g_pendingCreateOwner = kNoPending;
    }
    debug::logInfo("Horse P%u created", id);
#endif
}

void onCreateFailed(PlayerId id) {
#if TARGET_PC
    if (g_pendingCreateOwner == id) {
        g_pendingCreateOwner = kNoPending;
    }
    if (isValidPlayer(id)) {
        g_meta[id].createRequested = false;
        if (getOwnedHorse(id) == nullptr) {
            slot(id).summoned = false;
        }
    }
#else
    (void)id;
#endif
}

void onHorseDestroyed(daHorse_c* horse) {
    if (horse == nullptr) {
        return;
    }
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (getOwnedHorse(i) == horse) {
            slot(i).mounted = false;
            dComIfGp_setHorseActor(i, nullptr);
#if TARGET_PC
            clearMeta(i, /*keepPending=*/false);
#endif
            return;
        }
    }
}

void setRestart(PlayerId id, const char* stage, const cXyz& pos, s16 yaw, s8 room) {
    if (!isValidPlayer(id)) {
        return;
    }
    auto& s = slot(id);
    if (stage != nullptr) {
        s.lastKnownStage = stage;
    }
    s.lastKnownPosition = pos;
    s.lastKnownYaw = yaw;
    s.lastKnownRoom = room;
}

ScopedHorseOwnerContext::ScopedHorseOwnerContext(const daHorse_c& horse) {
#if TARGET_PC
    const PlayerId owner = ownerOf(&horse);
    ContextFrame frame{};
    frame.player = owner;
    frame.view = owner;
    ctx_.emplace(frame);
#else
    (void)horse;
#endif
}

const char* queryKindName(HorseQueryKind kind) {
    switch (kind) {
    case HorseQueryKind::OwnedHorse:
        return "OWNED_HORSE";
    case HorseQueryKind::MountedHorse:
        return "MOUNTED_HORSE";
    case HorseQueryKind::ActualCollidingActor:
        return "ACTUAL_COLLIDING_ACTOR";
    case HorseQueryKind::EventHorse:
        return "EVENT_HORSE";
    case HorseQueryKind::Player0Compatibility:
        return "PLAYER0_COMPATIBILITY";
    case HorseQueryKind::UnsafeUnresolved:
        return "UNSAFE_UNRESOLVED";
    }
    return "UNSAFE_UNRESOLVED";
}

}  // namespace dusk::coop::horses

#if TARGET_PC

namespace dusk::coop::horses {

daHorse_c* resolveForContextBridge() { return resolveForContext(); }

}  // namespace dusk::coop::horses

#endif  // TARGET_PC
