#include "dusk/coop/coop_render.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_accessors.h"
#include "dusk/coop/coop_camera.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_forms.h"

#include "SSystem/SComponent/c_angle.h"
#include "SSystem/SComponent/c_counter.h"
#include "SSystem/SComponent/c_math.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"
#include "d/d_drawlist.h"
#include "d/d_kankyo.h"
#include "f_op/f_op_actor.h"
#include "f_op/f_op_camera_mng.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_lib.h"
#include "m_Do/m_Do_mtx.h"
#include "m_Do/m_Do_ext.h"
#include "JSystem/J3DGraphBase/J3DSys.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JKernel/JKRHeap.h"

#include <gx.h>
#include <dolphin/mtx.h>

#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace dusk::coop::render {
namespace {

bool g_effectsDisabled = false;
uint64_t g_lastHash = 0;
uint32_t g_simTicks = 0;
uint32_t g_lastPassCount = 1;
uint8_t g_forcedViewCount = 0;
bool g_sameCameraSplit = false;
bool g_dualCameraComposite = false;

std::array<dDlst_window_c, MAX_LOCAL_VIEWS> g_windowSidecar{};
bool g_savedPrimaryViewport = false;
view_port_class g_primaryViewportBackup{};

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
ResTIMG* g_view1Timg = nullptr;
void* g_view1Tex = nullptr;
TGXTexObj g_view1TexObj{};
bool g_view1Captured = false;
bool g_view0Captured = false;
bool g_view1FromAlignedAlloc = false;

bool ensureView1CaptureBuffer() {
    if (g_view1Timg != nullptr && g_view1Tex != nullptr) {
        return true;
    }
    if (mDoGph_gInf_c::m_fullFrameBufferTimg == nullptr) {
        return false;
    }
    const u16 w = mDoGph_gInf_c::m_fullFrameBufferTimg->width;
    const u16 h = mDoGph_gInf_c::m_fullFrameBufferTimg->height;
    const u32 format = mDoGph_gInf_c::m_fullFrameBufferTimg->format;
    const u32 rawSize = GXGetTexBufferSize(w, h, format, GX_FALSE, 0) + 0x20;
    // aligned_alloc requires size multiple of alignment.
    const u32 bufferSize = (rawSize + 0x1fu) & ~0x1fu;

    // PC builds often leave JKRHeap::sRootHeap2 null (stub in dusk/stubs.cpp). Prefer
    // game/archive/current heaps, then posix aligned_alloc.
    void* mem = nullptr;
    g_view1FromAlignedAlloc = false;
    JKRExpHeap* heap = mDoExt_getArchiveHeap();
    if (heap == nullptr) {
        heap = mDoExt_getGameHeap();
    }
    if (heap != nullptr) {
        mem = heap->alloc(bufferSize, 0x20);
    }
    if (mem == nullptr) {
        JKRHeap* cur = JKRHeap::getCurrentHeap();
        if (cur != nullptr) {
            mem = cur->alloc(bufferSize, 0x20);
        }
    }
    if (mem == nullptr) {
        mem = std::aligned_alloc(0x20, bufferSize);
        if (mem != nullptr) {
            g_view1FromAlignedAlloc = true;
        }
    }
    if (mem == nullptr) {
        debug::logError("dual composite: failed to allocate view1 capture buffer (%u bytes)",
                        bufferSize);
        return false;
    }

    ResTIMG* timg = static_cast<ResTIMG*>(mem);
    std::memset(timg, 0, bufferSize);
    timg->format = format;
    timg->alphaEnabled = false;
    timg->width = w;
    timg->height = h;
    timg->minFilter = GX_LINEAR;
    timg->magFilter = GX_LINEAR;
    timg->mipmapCount = 1;
    timg->imageOffset = 0x20;
    g_view1Timg = timg;
    g_view1Tex = reinterpret_cast<char*>(timg) + sizeof(ResTIMG);
    debug::logInfo("dual composite: view1 capture buffer ready (%u bytes, aligned_alloc=%d)",
                   bufferSize, g_view1FromAlignedAlloc ? 1 : 0);
    return true;
}
#endif

// ISSUE: getCameraProcess(view) can be non-null while init_phase2 is still waiting
// (proxy not ready / floor check). Accessing mCamera before field_0xb0c==1 is UB.
// Dual composite must wait; stay on Gate A same-camera L/R blit until then.
bool secondaryCameraInitialized(ViewId view) {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    if (view == 0) {
        return true;
    }
    camera_class* cam = getCameraProcess(view);
    if (cam == nullptr) {
        return false;
    }
    auto* process = reinterpret_cast<camera_process_class*>(cam);
    return process->mCamera.field_0xb0c != 0;
#else
    (void)view;
    return false;
#endif
}

bool camerasReadyForDual() {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    return g_dualCameraComposite && isEnabled() && secondaryCameraInitialized(1);
#else
    return false;
#endif
}

struct ActorClassEntry {
    s16 procName;
    ActorDrawClass clas;
};

// Seed classification for Gate A. Expand via evidence notes as audits land.
// DependentMutating entries must not re-run draw prep per view until refactored.
constexpr ActorClassEntry kActorClassTable[] = {
    {fpcNm_CAMERA_e, ActorDrawClass::DependentPure},
    {fpcNm_CAMERA2_e, ActorDrawClass::DependentPure},

    {fpcNm_Obj_Flag_e, ActorDrawClass::DependentPure},
    {fpcNm_Obj_Flag2_e, ActorDrawClass::DependentPure},
    {fpcNm_Obj_Flag3_e, ActorDrawClass::DependentPure},
    {fpcNm_Obj_Yousei_e, ActorDrawClass::DependentPure},
    {fpcNm_GRASS_e, ActorDrawClass::DependentPure},

    {fpcNm_KYTAG03_e, ActorDrawClass::DependentPure},
    {fpcNm_KYTAG04_e, ActorDrawClass::DependentPure},
    {fpcNm_KYTAG10_e, ActorDrawClass::DependentPure},
    {fpcNm_KYEFF_e, ActorDrawClass::DependentPure},
    {fpcNm_KYEFF2_e, ActorDrawClass::DependentPure},

    {fpcNm_METER2_e, ActorDrawClass::Independent},
    {fpcNm_MENUWINDOW_e, ActorDrawClass::Independent},
    {fpcNm_MSG_OBJECT_e, ActorDrawClass::Independent},
    {fpcNm_GAMEOVER_e, ActorDrawClass::Independent},
    {fpcNm_TIMER_e, ActorDrawClass::Independent},

    {fpcNm_ALINK_e, ActorDrawClass::Independent},
    {fpcNm_MIDNA_e, ActorDrawClass::Independent},
    {fpcNm_HORSE_e, ActorDrawClass::Independent},

    // Demo/camera-copy paths that mutate actor fields from camera 0.
    {fpcNm_E_RDY_e, ActorDrawClass::DependentMutating},
    {fpcNm_E_PZ_e, ActorDrawClass::DependentMutating},
    {fpcNm_E_MK_e, ActorDrawClass::DependentMutating},
    {fpcNm_E_HZELDA_e, ActorDrawClass::DependentMutating},
    {fpcNm_B_DS_e, ActorDrawClass::DependentMutating},
    {fpcNm_B_GND_e, ActorDrawClass::DependentMutating},
};

uint8_t effectiveViewCount() {
    if (!isEnabled()) {
        return 1;
    }
    if (g_forcedViewCount > 1) {
        return g_forcedViewCount;
    }
    // Gate B dual composite: two full-frame passes (not tiled).
    if (camerasReadyForDual()) {
        return 2;
    }
    // Same-camera split presents from a single world pass.
    if (g_sameCameraSplit) {
        return 1;
    }
    const uint8_t n = runtime().activeViewCount;
    return n < 1 ? 1 : n;
}

void backupPrimaryViewportIfNeeded() {
    if (g_savedPrimaryViewport) {
        return;
    }
    dDlst_window_c* primary = dComIfGp_getWindow(0);
    if (primary == nullptr) {
        return;
    }
    g_primaryViewportBackup = *primary->getViewPort();
    g_savedPrimaryViewport = true;
}

void restorePrimaryViewportIfNeeded() {
    if (!g_savedPrimaryViewport) {
        return;
    }
    dDlst_window_c* primary = dComIfGp_getWindow(0);
    if (primary != nullptr) {
        *primary->getViewPort() = g_primaryViewportBackup;
        primary->setCameraID(0);
    }
    g_savedPrimaryViewport = false;
}

void syncViewportSidecars() {
    const uint8_t count = effectiveViewCount();
    g_lastPassCount = count;
    if (count <= 1) {
        restorePrimaryViewportIfNeeded();
        return;
    }

    backupPrimaryViewportIfNeeded();

    const f32 fbW = static_cast<f32>(FB_WIDTH);
    const f32 fbH = static_cast<f32>(FB_HEIGHT);
    const ViewAssignmentMode mode = runtime().viewMode;
    // Dual composite: every pass uses the full framebuffer (capture then L/R blit).
    // Tiled scissors black the Metal world path — never half-tile in this mode.
    const bool fullFramePasses = camerasReadyForDual() || g_dualCameraComposite;

    for (ViewId v = 0; v < count; ++v) {
        const ViewportRect rect =
            fullFramePasses ? ViewportRect{} : viewportFor(v, count, mode);
        dDlst_window_c* window = (v == 0) ? dComIfGp_getWindow(0) : &g_windowSidecar[v];
        applyViewportToWindow(window, rect, fbW, fbH);
        if (fullFramePasses) {
            window->setCameraID(static_cast<int>(v));
        } else {
            // Same-camera PoC: every sidecar points at camera slot 0 (never index >0).
            window->setCameraID(0);
        }
        window->setMode(2);
    }
}

uint64_t mixU64(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

uint64_t computeFrameStateHash() {
    uint64_t h = 0xcbf29ce484222325ULL;
    h = mixU64(h, g_Counter.mTimer);
    h = mixU64(h, g_Counter.mCounter0);
    h = mixU64(h, static_cast<uint64_t>(static_cast<int64_t>(g_Counter.mCounter1)));
    h = mixU64(h, g_simTicks);

    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player != nullptr) {
        u32 bits[3];
        std::memcpy(&bits[0], &player->current.pos.x, sizeof(u32));
        std::memcpy(&bits[1], &player->current.pos.y, sizeof(u32));
        std::memcpy(&bits[2], &player->current.pos.z, sizeof(u32));
        h = mixU64(h, bits[0]);
        h = mixU64(h, bits[1]);
        h = mixU64(h, bits[2]);
        h = mixU64(h, static_cast<uint64_t>(player->shape_angle.y));
    }
    return h;
}

PlayerId playerForView(ViewId view) {
    if (auto* route = cameraRoute(view)) {
        if (!route->trackedPlayers.empty()) {
            return route->trackedPlayers.front();
        }
        return route->inputOwner;
    }
    return static_cast<PlayerId>(view < MAX_LOCAL_PLAYERS ? view : 0);
}

ContextFrame makeFrame(ViewId view) {
    ContextFrame frame;
    frame.player = playerForView(view);
    frame.view = view;
    frame.enemyTarget = nullptr;
    return frame;
}

}  // namespace

ScopedWorldDrawPass::ScopedWorldDrawPass(ViewId view) : context_(makeFrame(view)) {
    if (!isEnabled()) {
        return;
    }

    prevWindow_ = g_dComIfG_gameInfo.play.mCurrentWindow;
    prevPlayView_ = g_dComIfG_gameInfo.play.mCurrentView;
    prevPlayViewport_ = g_dComIfG_gameInfo.play.mCurrentViewport;
    prevDrawView_ = dComIfGd_getView();
    prevDrawViewport_ = dComIfGd_getViewport();

    dDlst_window_c* window = resolveWindow(view);
    camera_process_class* camera = resolveCamera(view);
    view_port_class* port = window != nullptr ? window->getViewPort() : nullptr;

    if (window != nullptr) {
        dComIfGp_setCurrentWindow(window);
        dComIfGd_setWindow(window);
    }
    if (camera != nullptr) {
        dComIfGp_setCurrentView(&camera->view);
        dComIfGd_setView(&camera->view);
    }
    if (port != nullptr) {
        dComIfGp_setCurrentViewport(port);
        dComIfGd_setViewport(port);
        if (isMultiViewActive()) {
            GXSetViewport(port->x_orig, port->y_orig, port->width, port->height, port->near_z,
                          port->far_z);
            GXSetScissor(static_cast<u32>(port->scissor.x_orig),
                         static_cast<u32>(port->scissor.y_orig),
                         static_cast<u32>(port->scissor.width),
                         static_cast<u32>(port->scissor.height));
        }
    }

    // Gate I: senses post/env visualization only in views whose owner has senses.
    if (isMultiViewActive()) {
        dScnKy_env_light_c* env = dKy_getEnvlight();
        if (env != nullptr) {
            prevSensesEffect_ = env->now_senses_effect;
            prevSensesStrength_ = env->senses_effect_strength;
            sensesOverride_ = true;
            if (!forms::sensesActiveForView(view)) {
                env->now_senses_effect = 0;
                env->senses_effect_strength = 0.0f;
            } else if (env->now_senses_effect == 0) {
                // Owner has sidecar senses but global sim may still be off (secondary).
                env->now_senses_effect = 1;
                env->senses_effect_strength = 1.0f;
            }
        }
    }
    active_ = true;
}

ScopedWorldDrawPass::~ScopedWorldDrawPass() {
    if (!active_) {
        return;
    }
    if (sensesOverride_) {
        dScnKy_env_light_c* env = dKy_getEnvlight();
        if (env != nullptr) {
            env->now_senses_effect = prevSensesEffect_;
            env->senses_effect_strength = prevSensesStrength_;
        }
        sensesOverride_ = false;
    }
    if (prevWindow_ != nullptr) {
        dComIfGp_setCurrentWindow(prevWindow_);
        dComIfGd_setWindow(prevWindow_);
    }
    if (prevPlayView_ != nullptr) {
        dComIfGp_setCurrentView(prevPlayView_);
    }
    if (prevDrawView_ != nullptr) {
        dComIfGd_setView(prevDrawView_);
    }
    if (prevPlayViewport_ != nullptr) {
        dComIfGp_setCurrentViewport(prevPlayViewport_);
    }
    if (prevDrawViewport_ != nullptr) {
        dComIfGd_setViewport(prevDrawViewport_);
    }
}

void init() {
    restorePrimaryViewportIfNeeded();
    g_effectsDisabled = false;
    g_lastHash = 0;
    g_simTicks = 0;
    g_lastPassCount = 1;
    g_forcedViewCount = 0;
    g_sameCameraSplit = false;
    g_dualCameraComposite = false;
    g_windowSidecar = {};
    g_primaryViewportBackup = {};
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    g_view0Captured = false;
    g_view1Captured = false;
#endif
}

void reset() { init(); }

void beginFrame() {
    if (!isEnabled()) {
        return;
    }
    // Simulation advances once per game tick elsewhere; render must not bump counters.
    if (isMultiViewActive()) {
        syncViewportSidecars();
    } else {
        restorePrimaryViewportIfNeeded();
    }
}

void endFrame() {
    if (!isEnabled()) {
        return;
    }
    g_lastHash = computeFrameStateHash();
}

void noteSimulationTick() {
    if (!isEnabled()) {
        return;
    }
    ++g_simTicks;
}

bool drawViews() {
    if (!isEnabled() || effectiveViewCount() <= 1) {
        restorePrimaryViewportIfNeeded();
        g_lastPassCount = 1;
        return false;  // caller uses original single-view path
    }
    syncViewportSidecars();
    return true;
}

uint8_t worldDrawPassCount() {
    if (!isEnabled()) {
        return 1;
    }
    return effectiveViewCount();
}

bool isMultiViewActive() { return isEnabled() && effectiveViewCount() > 1; }

dDlst_window_c* resolveWindow(ViewId view) {
    if (!isEnabled() || view == 0 || !isMultiViewActive()) {
        return dComIfGp_getWindow(0);
    }
    COOP_ASSERT(view < MAX_LOCAL_VIEWS);
    return &g_windowSidecar[view];
}

camera_process_class* resolveCamera(ViewId view) {
    // Gate A same-camera PoC: camera 0 unless Gate B has a *fully initialized* secondary.
    // Never return an incomplete cam1 body, and never silently fall back to cam0 for
    // view>0 while dual composite is attempting — that stomps P0 lookat / stick yaw.
    if (isEnabled() && view > 0 && secondaryCameraInitialized(view)) {
        if (camera_class* secondary = getCameraProcess(view)) {
            return reinterpret_cast<camera_process_class*>(secondary);
        }
    }
    return dComIfGp_getCamera(0);
}

ViewportRect viewportFor(ViewId view, uint8_t activeViewCount, ViewAssignmentMode mode) {
    ViewportRect rect;
    // Dual composite presents via capture+blit; keep camera/window rects full-frame.
    if (g_dualCameraComposite) {
        return rect;
    }
    if (activeViewCount <= 1) {
        return rect;
    }

    uint8_t cols = 1;
    uint8_t rows = 1;
    switch (mode) {
    case ViewAssignmentMode::TwoByOneGrid:
        cols = 2;
        rows = 1;
        break;
    case ViewAssignmentMode::TwoByTwoGrid:
        cols = 2;
        rows = 2;
        break;
    case ViewAssignmentMode::ThreeByTwoGrid:
        cols = 3;
        rows = 2;
        break;
    case ViewAssignmentMode::FourByTwoGrid:
        cols = 4;
        rows = 2;
        break;
    case ViewAssignmentMode::OnePerPlayer:
    default:
        if (activeViewCount <= 2) {
            cols = 2;
            rows = 1;
        } else if (activeViewCount <= 4) {
            cols = 2;
            rows = 2;
        } else if (activeViewCount <= 6) {
            cols = 3;
            rows = 2;
        } else {
            cols = 4;
            rows = 2;
        }
        break;
    }

    const uint8_t col = static_cast<uint8_t>(view % cols);
    const uint8_t row = static_cast<uint8_t>(view / cols);
    rect.width = 1.0f / static_cast<f32>(cols);
    rect.height = 1.0f / static_cast<f32>(rows);
    rect.x = rect.width * static_cast<f32>(col);
    rect.y = rect.height * static_cast<f32>(row);
    return rect;
}

void applyViewportToWindow(dDlst_window_c* window, const ViewportRect& norm, f32 fbWidth,
                           f32 fbHeight) {
    if (window == nullptr) {
        return;
    }
    const f32 x = norm.x * fbWidth;
    const f32 y = norm.y * fbHeight;
    const f32 w = norm.width * fbWidth;
    const f32 h = norm.height * fbHeight;
    window->setViewPort(x, y, w, h, 0.0f, 1.0f);
    window->setScissor(x, y, w, h);
}

ActorDrawClass classifyActor(s16 procName) {
    for (const ActorClassEntry& entry : kActorClassTable) {
        if (entry.procName == procName) {
            return entry.clas;
        }
    }
    return ActorDrawClass::Independent;
}

uint64_t lastFrameStateHash() { return g_lastHash; }
uint32_t simulationTickCounter() { return g_simTicks; }
uint32_t lastWorldDrawPassCount() { return g_lastPassCount; }

void setIncompatibleEffectsDisabled(bool disabled) { g_effectsDisabled = disabled; }
bool incompatibleEffectsDisabled() { return g_effectsDisabled; }

bool shouldSkipEffect(IncompatibleEffect effect) {
    if (!isMultiViewActive()) {
        return false;
    }
    if (!g_effectsDisabled) {
        return false;
    }
    switch (effect) {
    case IncompatibleEffect::MotionBlur:
    case IncompatibleEffect::DepthOfField:
    case IncompatibleEffect::FrameBufferCapture:
    case IncompatibleEffect::Bloom:
    case IncompatibleEffect::FullFrameFade:
    case IncompatibleEffect::MirrorModeCopy:
    case IncompatibleEffect::ScreenSpaceParticles:
        return true;
    }
    return true;
}

void setForcedViewCount(uint8_t count) {
    if (count > MAX_LOCAL_VIEWS) {
        count = static_cast<uint8_t>(MAX_LOCAL_VIEWS);
    }
    g_forcedViewCount = count;
    if (!isEnabled() || count <= 1) {
        // Leaving multi-view: put window 0 back to the pre-tile full framebuffer.
        restorePrimaryViewportIfNeeded();
        if (isEnabled()) {
            runtime().activeViewCount = 1;
        }
        g_lastPassCount = 1;
        return;
    }
    runtime().activeViewCount = count;
}

uint8_t forcedViewCount() { return g_forcedViewCount; }

void setSameCameraSplitEnabled(bool enabled) { g_sameCameraSplit = enabled; }

bool sameCameraSplitEnabled() {
    return isEnabled() && g_sameCameraSplit && runtime().joinedPlayerCount >= 2;
}

void setDualCameraCompositeEnabled(bool enabled) { g_dualCameraComposite = enabled; }

bool dualCameraCompositeEnabled() {
    return isEnabled() && g_dualCameraComposite && runtime().joinedPlayerCount >= 2;
}

bool dualCameraCompositeReady() { return camerasReadyForDual() && dualCameraCompositeEnabled(); }

bool usesHorizontalSplitPresent() {
    return sameCameraSplitEnabled() || dualCameraCompositeEnabled();
}

f32 presentationPaneAspect() {
    const f32 full = mDoGph_gInf_c::getAspect();
    // L/R blit halves horizontal FOV relative to a full-frame capture.
    // ISSUE: apply only to painter projMtx — never write into camera->view.aspect
    // (see preparation() stick-yaw note in d_camera.cpp).
    return usesHorizontalSplitPresent() ? (full * 0.5f) : full;
}

void bindPainterCameraView(ViewId view, camera_process_class* camera) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)view;
    (void)camera;
#else
    if (camera == nullptr) {
        return;
    }

    // Task 02 / Gate B: presentation binds matrices from the camera the scheduler owns.
    // Do NOT write camera->view.aspect or mutate cam0 chase state here — that flips P0
    // stick yaw when preparation() fights a half-pane aspect write.
    //
    // Safety: if this pass resolved a real secondary camera but its lookat still sits
    // on P0 (interp stub / late init), re-anchor lookat onto the tracked actor for this
    // raster only. Sim body (mControlledYaw etc.) stays owned by dCamera_c::Run().
    if (view > 0 && dualCameraCompositeReady() && secondaryCameraInitialized(view) &&
        reinterpret_cast<camera_process_class*>(getCameraProcess(view)) == camera) {
        PlayerId tracked = static_cast<PlayerId>(view);
        if (auto* route = cameraRoute(view)) {
            if (!route->trackedPlayers.empty()) {
                tracked = route->trackedPlayers.front();
            } else {
                tracked = route->inputOwner;
            }
        }
        fopAc_ac_c* actor = getPlayerActor(tracked);
        if (actor != nullptr) {
            cXyz center = actor->attention_info.position;
            if (center.y == 0.0f && actor->eyePos.y != 0.0f) {
                center = actor->eyePos;
            }
            if (center.x == 0.0f && center.y == 0.0f && center.z == 0.0f) {
                center = actor->current.pos;
                center.y += 150.0f;
            }

            cXyz eye = camera->view.lookat.eye;
            cXyz delta = eye - center;
            f32 distSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
            if (distSq < 1.0f || !std::isfinite(distSq)) {
                cSAngle pitch(static_cast<s16>(0x0E00));
                cSAngle yaw(static_cast<s16>(cSAngle(actor->shape_angle.y).Inv()));
                cSGlobe behind(280.0f, pitch, yaw);
                eye = center + behind.Xyz();
            } else {
                const f32 dist = std::sqrt(distSq);
                eye = center + delta * (280.0f / dist);
            }

            camera->view.lookat.center = center;
            camera->view.lookat.eye = eye;
            camera->view.lookat.up.set(0.0f, 1.0f, 0.0f);

            static bool sLoggedBind = false;
            if (!sLoggedBind) {
                debug::logInfo(
                    "bindPainterCameraView: view=%u tracked=P%u center=(%.1f,%.1f,%.1f) "
                    "eye=(%.1f,%.1f,%.1f) cam0eye=(%.1f,%.1f,%.1f)",
                    view, tracked, center.x, center.y, center.z, eye.x, eye.y, eye.z,
                    dComIfGp_getCamera(0) ? dComIfGp_getCamera(0)->view.lookat.eye.x : 0.0f,
                    dComIfGp_getCamera(0) ? dComIfGp_getCamera(0)->view.lookat.eye.y : 0.0f,
                    dComIfGp_getCamera(0) ? dComIfGp_getCamera(0)->view.lookat.eye.z : 0.0f);
                sLoggedBind = true;
            }
        } else {
            debug::logWarn("bindPainterCameraView: view=%u has no tracked actor (P%u)", view,
                           tracked);
        }
    }

    const f32 aspect =
        usesHorizontalSplitPresent() ? presentationPaneAspect() : camera->view.aspect;
    C_MTXPerspective(camera->view.projMtx, camera->view.fovy, aspect, camera->view.near_,
                     camera->view.far_);
    mDoMtx_lookAt(camera->view.viewMtx, &camera->view.lookat.eye, &camera->view.lookat.center,
                  &camera->view.lookat.up, camera->view.bank);
    MTXCopy(camera->view.viewMtx, camera->view.viewMtxNoTrans);
    camera->view.viewMtxNoTrans[0][3] = 0.0f;
    camera->view.viewMtxNoTrans[1][3] = 0.0f;
    camera->view.viewMtxNoTrans[2][3] = 0.0f;
    cMtx_concatProjView(camera->view.projMtx, camera->view.viewMtx, camera->view.projViewMtx);
    cMtx_inverse(camera->view.viewMtx, camera->view.invViewMtx);
#endif
}

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
namespace {

void setupSplitCompositeState() {
    GXSetNumChans(0);
    GXSetNumIndStages(0);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, 0x3C);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GXSetZCompLoc(GX_ENABLE);
    GXSetZMode(GX_DISABLE, GX_ALWAYS, GX_DISABLE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, g_clearColor);
    GXSetFogRangeAdj(GX_DISABLE, 0, nullptr);
    GXSetCullMode(GX_CULL_NONE);
    GXSetDither(GX_ENABLE);

    Mtx44 ortho;
    MTXOrtho(ortho, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 10.0f);
    GXSetProjection(ortho, GX_ORTHOGRAPHIC);
    GXLoadPosMtxImm(cMtx_getIdentity(), GX_PNMTX0);
    GXSetCurrentMtx(0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_RGB8, 0);
}

void blitSplitPane(f32 x, f32 y, f32 w, f32 h) {
    GXSetViewport(x, y, w, h, 0.0f, 1.0f);
    GXSetScissor(static_cast<u32>(x), static_cast<u32>(y), static_cast<u32>(w),
                 static_cast<u32>(h));
    mDoGph_drawFilterQuad(1, 1);
}

}  // namespace
#endif

void captureViewToSlot(int slot) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)slot;
#else
    if (!dualCameraCompositeReady()) {
        return;
    }
    if (mDoGph_gInf_c::m_fullFrameBufferTimg == nullptr ||
        mDoGph_gInf_c::m_fullFrameBufferTex == nullptr) {
        return;
    }

    const f32 fbW = mDoGph_gInf_c::getWidth();
    const f32 fbH = mDoGph_gInf_c::getHeight();
    if (fbW < 2.0f || fbH < 1.0f) {
        return;
    }

    void* dest = nullptr;
    if (slot == 0) {
        dest = mDoGph_gInf_c::m_fullFrameBufferTex;
    } else if (slot == 1) {
        if (!ensureView1CaptureBuffer()) {
            // Stay on same-camera split rather than crashing every painter frame.
            debug::logError(
                "dual composite: view1 buffer unavailable — disabling dual, same-camera fallback");
            g_dualCameraComposite = false;
            g_view0Captured = false;
            g_view1Captured = false;
            return;
        }
        dest = g_view1Tex;
    } else {
        return;
    }

    // Clear EFB (color+depth) after capturing pass 0 so pass 1 starts clean.
    // JFWDisplay::clearEfb is stubbed on TARGET_PC — GXCopyTex clear is the reliable path.
    const bool clearAfter = (slot == 0);
    if (clearAfter) {
        GXSetColorUpdate(GX_ENABLE);
        GXSetAlphaUpdate(GX_ENABLE);
        GXSetZMode(GX_ENABLE, GX_ALWAYS, GX_ENABLE);
        GXSetCopyClear(mDoGph_gInf_c::getBackColor(), GX_MAX_Z24);
    }

    GXSetTexCopySrc(0, 0, static_cast<u16>(fbW), static_cast<u16>(fbH));
    GXSetTexCopyDst(static_cast<u16>(fbW), static_cast<u16>(fbH),
                    static_cast<GXTexFmt>(mDoGph_gInf_c::m_fullFrameBufferTimg->format), 0);
    GXCopyTex(dest, clearAfter ? GX_TRUE : GX_FALSE);
    GXPixModeSync();
    GXInvalidateTexAll();

    if (slot == 0) {
        g_view0Captured = true;
    } else {
        g_view1Captured = true;
    }
#endif
}

bool presentDualCameraSplit() {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    return false;
#else
    if (!dualCameraCompositeReady() || !g_view0Captured || !g_view1Captured) {
        return false;
    }
    if (mDoGph_gInf_c::m_fullFrameBufferTimg == nullptr || g_view1Timg == nullptr) {
        return false;
    }

    const f32 fbW = mDoGph_gInf_c::getWidth();
    const f32 fbH = mDoGph_gInf_c::getHeight();
    if (fbW < 2.0f || fbH < 1.0f) {
        return false;
    }

    setupSplitCompositeState();

    mDoLib_setResTimgObj(mDoGph_gInf_c::m_fullFrameBufferTimg,
                         &mDoGph_gInf_c::m_fullFrameBufferTexObj, 0, nullptr);
    GXLoadTexObj(&mDoGph_gInf_c::m_fullFrameBufferTexObj, GX_TEXMAP0);
    const f32 halfW = fbW * 0.5f;
    blitSplitPane(0.0f, 0.0f, halfW, fbH);

    mDoLib_setResTimgObj(g_view1Timg, &g_view1TexObj, 0, nullptr);
    GXLoadTexObj(&g_view1TexObj, GX_TEXMAP0);
    blitSplitPane(halfW, 0.0f, halfW, fbH);

    GXSetViewport(0.0f, 0.0f, fbW, fbH, 0.0f, 1.0f);
    GXSetScissor(0, 0, static_cast<u32>(fbW), static_cast<u32>(fbH));
    j3dSys.reinitGX();

    g_view0Captured = false;
    g_view1Captured = false;
    return true;
#endif
}

void presentSameCameraSplit() {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    return;
#else
    if (!sameCameraSplitEnabled()) {
        return;
    }
    if (mDoGph_gInf_c::m_fullFrameBufferTimg == nullptr ||
        mDoGph_gInf_c::m_fullFrameBufferTex == nullptr) {
        return;
    }

    // Capture the single full-frame world render, then show it in both panes.
    const f32 fbW = mDoGph_gInf_c::getWidth();
    const f32 fbH = mDoGph_gInf_c::getHeight();
    if (fbW < 2.0f || fbH < 1.0f) {
        return;
    }

    GXSetTexCopySrc(0, 0, static_cast<u16>(fbW), static_cast<u16>(fbH));
    GXSetTexCopyDst(static_cast<u16>(fbW), static_cast<u16>(fbH),
                    static_cast<GXTexFmt>(mDoGph_gInf_c::m_fullFrameBufferTimg->format), 0);
    GXCopyTex(mDoGph_gInf_c::m_fullFrameBufferTex, 0);
    GXPixModeSync();
    GXInvalidateTexAll();

    mDoLib_setResTimgObj(mDoGph_gInf_c::m_fullFrameBufferTimg, &mDoGph_gInf_c::m_fullFrameBufferTexObj,
                         0, nullptr);
    GXLoadTexObj(&mDoGph_gInf_c::m_fullFrameBufferTexObj, GX_TEXMAP0);

    setupSplitCompositeState();

    const f32 halfW = fbW * 0.5f;
    blitSplitPane(0.0f, 0.0f, halfW, fbH);
    blitSplitPane(halfW, 0.0f, halfW, fbH);

    // Restore full framebuffer for HUD / subsequent passes.
    GXSetViewport(0.0f, 0.0f, fbW, fbH, 0.0f, 1.0f);
    GXSetScissor(0, 0, static_cast<u32>(fbW), static_cast<u32>(fbH));
    j3dSys.reinitGX();
#endif
}

}  // namespace dusk::coop::render
