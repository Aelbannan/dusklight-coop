#include "dusk/coop/coop_camera.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_accessors.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_render.h"

#include "SSystem/SComponent/c_malloc.h"
#include "d/d_com_inf_game.h"
#include "d/d_drawlist.h"
#include "f_op/f_op_camera_mng.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_graphic.h"

#include <array>
#include <cstring>

namespace dusk::coop::camera {
namespace {

std::array<bool, MAX_LOCAL_VIEWS> g_active{};
std::array<bool, MAX_LOCAL_VIEWS> g_createRequested{};

bool validIndex(int idx) { return idx >= 0 && idx < static_cast<int>(MAX_LOCAL_VIEWS); }

uint8_t computeViewSpan() {
    uint8_t span = 1;
    for (ViewId i = 0; i < MAX_LOCAL_VIEWS; ++i) {
        if (g_active[i]) {
            span = static_cast<uint8_t>(i + 1);
        }
    }
    return span;
}

void clearRouteSlot(ViewId id, bool keepMapping) {
    auto* route = cameraRoute(id);
    if (!route) {
        return;
    }
    route->process = nullptr;
    route->processId = fpcM_ERROR_PROCESS_ID_e;
    if (!keepMapping) {
        route->trackedPlayers.clear();
        route->inputOwner = 0;
        route->attentionOwner = 0;
    }
    g_createRequested[id] = false;
}

void syncPrimaryRoute() {
    auto* route = cameraRoute(0);
    if (!route) {
        return;
    }
    route->view = 0;
    route->process = reinterpret_cast<camera_class*>(dComIfGp_getCamera(0));
    route->processId = fopCamM_GetID(0);
    if (route->processId == fpcM_ERROR_PROCESS_ID_e && route->process != nullptr) {
        route->processId = fpcM_GetID(route->process);
    }
    route->inputOwner = 0;
    route->attentionOwner = 0;
    if (route->trackedPlayers.empty()) {
        route->trackedPlayers.push_back(0);
    }
    g_active[0] = true;
}

void prepareNativeMapping(ViewId id, PlayerId owner) {
    auto* route = cameraRoute(id);
    if (!route) {
        return;
    }
    route->view = id;
    route->inputOwner = owner;
    route->attentionOwner = owner;
    route->trackedPlayers = {owner};
    // Share param file with primary so initialize() can load camtype.dat.
    char* paramFileName = const_cast<char*>(dComIfGp_getCameraParamFileName(0));

    // Populate the engine's native slot before the camera process initializes.
    dComIfGp_setCameraInfo(id, nullptr, id, owner, -1);
    dComIfGp_setCameraParamFileName(id, paramFileName);
    dComIfGp_setCameraZoomScale(id, 1.0f);
    dComIfGp_setCameraZoomForcus(id, 1.0f);

    assignWindow(id);
}

bool requestSecondaryCreate(ViewId id) {
    if (id == 0 || !validIndex(id)) {
        return false;
    }
    if (g_createRequested[id]) {
        return true;
    }
    // Already have a live process registered at this sparse slot.
    if (fopCamM_GetID(id) != fpcM_ERROR_PROCESS_ID_e) {
        g_createRequested[id] = true;
        return true;
    }

    fopCamM_prm_class* params =
        static_cast<fopCamM_prm_class*>(cMl::memalignB(-4, sizeof(fopCamM_prm_class)));
    if (params == nullptr) {
        debug::logError("ensureCameras: failed to allocate camera params for view %u", id);
        return false;
    }
    std::memset(params, 0, sizeof(*params));
    // fopCam_Create does `fpcM_SetParam(a_this, *append)` with a raw u32* read of the
    // append blob — it does NOT go through BE(u32). Writing via params->base.parameters
    // (BE) stores a byteswapped value, so camera_id becomes 0x01000000 for id==1 and
    // init_phase1 reads this raw process parameter. Store it host-endian rather than
    // through the big-endian wrapper so every native camera slot receives the right ID.
    *reinterpret_cast<u32*>(params) = id;

    const fpc_ProcID pid = fopCamM_Create(static_cast<int>(id), fpcNm_CAMERA_e, params);
    if (pid == fpcM_ERROR_PROCESS_ID_e) {
        debug::logError("ensureCameras: fopCamM_Create failed for view %u", id);
        return false;
    }

    if (auto* route = cameraRoute(id)) {
        route->processId = pid;
        route->process = nullptr;  // resolved asynchronously via tick/ensureCameras
    }
    g_createRequested[id] = true;
    return true;
}

void resolveProcessPointer(ViewId id) {
    auto* route = cameraRoute(id);
    if (!route) {
        return;
    }
    fpc_ProcID pid = route->processId;
    if (pid == fpcM_ERROR_PROCESS_ID_e) {
        pid = fopCamM_GetID(static_cast<int>(id));
        route->processId = pid;
    }
    if (pid == fpcM_ERROR_PROCESS_ID_e) {
        return;
    }
    base_process_class* proc = fpcM_SearchByID(pid);
    if (proc != nullptr) {
        route->process = reinterpret_cast<camera_class*>(proc);
        setCameraProcess(id, route->process);
    }
}

}  // namespace

void init() {
    g_active = {};
    g_createRequested = {};
    g_active[0] = true;
    if (auto* route = cameraRoute(0)) {
        route->view = 0;
        route->inputOwner = 0;
        route->attentionOwner = 0;
        route->trackedPlayers = {0};
        route->processId = fpcM_ERROR_PROCESS_ID_e;
        route->process = nullptr;
    }
}

void reset() { init(); }

void tick() {
    syncPrimaryRoute();
    for (ViewId i = 1; i < MAX_LOCAL_VIEWS; ++i) {
        if (g_active[i] && g_createRequested[i]) {
            resolveProcessPointer(i);
        }
    }

#if TARGET_PC
    // Gate B: dualCameraCompositeReady requires field_0xb0c (not just pid).
    static bool sLoggedDualReady = false;
    if (render::dualCameraCompositeReady()) {
        if (!sLoggedDualReady) {
            debug::logInfo("Gate B: cam1 initialized — dual-camera composite active");
            sLoggedDualReady = true;
        }
    }
#endif
}

bool ensureCameras(uint8_t count) {
    if (count == 0 || count > MAX_LOCAL_VIEWS) {
        return false;
    }

    // Route construction before process creation (design: task 03).
    syncPrimaryRoute();
    for (ViewId i = 0; i < count; ++i) {
        g_active[i] = true;
        const PlayerId owner = static_cast<PlayerId>(i);
        if (i == 0) {
            if (auto* route = cameraRoute(0)) {
                route->view = 0;
                if (route->trackedPlayers.empty()) {
                    route->trackedPlayers.push_back(0);
                }
                route->inputOwner = 0;
                route->attentionOwner = 0;
            }
            continue;
        }
        prepareNativeMapping(i, owner);
        if (!requestSecondaryCreate(i)) {
            return false;
        }
        resolveProcessPointer(i);
    }

    // Contiguous ensure: tear down anything at or above `count` (sparse leave uses removeCameraSlot).
    for (ViewId i = count; i < MAX_LOCAL_VIEWS; ++i) {
        if (g_active[i] || g_createRequested[i] ||
            fopCamM_GetID(static_cast<int>(i)) != fpcM_ERROR_PROCESS_ID_e) {
            destroyCamera(i);
        } else {
            g_active[i] = false;
        }
    }

    runtime().activeViewCount = computeViewSpan();
    return true;
}

void destroyCamera(ViewId id) {
    if (id == 0 || !isValidView(id)) {
        return;  // never destroy primary through this path
    }

    // fopCamM_Delete schedules process teardown; destructor side effects are guarded in dCamera_c.
    fopCamM_Delete(static_cast<int>(id));
    clearRouteSlot(id, /*keepMapping=*/false);
    g_active[id] = false;
    runtime().activeViewCount = computeViewSpan();
}

void destroySecondaryCameras() {
    for (ViewId i = 1; i < MAX_LOCAL_VIEWS; ++i) {
        if (g_active[i] || g_createRequested[i] || fopCamM_GetID(static_cast<int>(i)) != fpcM_ERROR_PROCESS_ID_e) {
            destroyCamera(i);
        }
    }
    runtime().activeViewCount = 1;
}

bool assignInputOwner(ViewId id, PlayerId owner) {
    auto* route = cameraRoute(id);
    if (!route || !isValidPlayer(owner)) {
        return false;
    }
    route->inputOwner = owner;
    dComIfGp_setCameraInfo(id, reinterpret_cast<camera_class*>(dComIfGp_getCamera(id)), id,
                           owner, -1);
    return true;
}

bool assignTrackedPlayer(ViewId id, PlayerId player) {
    auto* route = cameraRoute(id);
    if (!route || !isValidPlayer(player)) {
        return false;
    }
    route->trackedPlayers = {player};
    dComIfGp_setCameraInfo(id, reinterpret_cast<camera_class*>(dComIfGp_getCamera(id)), id,
                           player, -1);
    return true;
}

bool assignAttentionOwner(ViewId id, PlayerId owner) {
    auto* route = cameraRoute(id);
    if (!route || !isValidPlayer(owner)) {
        return false;
    }
    route->attentionOwner = owner;
    return true;
}

bool assignWindow(ViewId id) {
    if (!isValidView(id)) {
        return false;
    }
    auto* route = cameraRoute(id);
    if (!route) {
        return false;
    }

    uint8_t span = computeViewSpan();
    if (span < 1) {
        span = 1;
    }
    if (runtime().activeViewCount > span) {
        span = runtime().activeViewCount;
    }
    const auto vp = render::viewportFor(id, span, runtime().viewMode);

    const f32 fbW = static_cast<f32>(FB_WIDTH);
    const f32 fbH = static_cast<f32>(FB_HEIGHT);
    const f32 x = vp.x * fbW;
    const f32 y = vp.y * fbH;
    const f32 w = vp.width * fbW;
    const f32 h = vp.height * fbH;

    if (id == 0) {
        dComIfGp_setWindow(0, x, y, w, h, 0.0f, 1.0f, 0, 2);
    } else {
        dComIfGp_setWindow(id, x, y, w, h, 0.0f, 1.0f, id, 2);
    }
    return true;
}

bool removeCameraSlot(ViewId id) {
    if (id == 0 || !isValidView(id)) {
        return false;
    }
    // Noncontiguous: clear this slot only; do not compact or shift higher indices.
    destroyCamera(id);
    return true;
}

bool rejoinCameraSlot(ViewId id, PlayerId owner) {
    if (id == 0 || !isValidView(id) || !isValidPlayer(owner)) {
        return false;
    }
    g_active[id] = true;
    prepareNativeMapping(id, owner);
    if (!requestSecondaryCreate(id)) {
        g_active[id] = false;
        return false;
    }
    resolveProcessPointer(id);
    runtime().activeViewCount = computeViewSpan();
    return true;
}

uint8_t activeCameraCount() {
    uint8_t n = 0;
    for (bool a : g_active) {
        if (a) {
            ++n;
        }
    }
    return n;
}

bool isCameraActive(ViewId id) { return isValidView(id) && g_active[id]; }

bool isSecondaryCameraBody(const void* dCameraBody) {
#if TARGET_PC
    if (dCameraBody == nullptr) {
        return false;
    }
    // dCamera_c::CameraID() is stored at the documented offset; prefer walking active routes.
    for (ViewId i = 1; i < MAX_LOCAL_VIEWS; ++i) {
        auto* route = cameraRoute(i);
        if (!route || route->process == nullptr) {
            continue;
        }
        auto* process = reinterpret_cast<camera_process_class*>(route->process);
        if (&process->mCamera == dCameraBody) {
            return true;
        }
    }
    return false;
#else
    (void)dCameraBody;
    return false;
#endif
}

}  // namespace dusk::coop::camera
