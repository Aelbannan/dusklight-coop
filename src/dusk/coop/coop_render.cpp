#include "dusk/coop/coop_render.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_accessors.h"
#include "dusk/coop/coop_camera.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_event.h"
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

uint64_t g_lastHash = 0;
uint32_t g_simTicks = 0;
uint32_t g_lastPassCount = 1;

// ── Presentation override state ────────────────────────────────────────────
// Active while an initiator-owned conversation is running.
static struct {
    bool active = false;
    ViewId owner = 0;
    uint8_t savedViewCount = 0;
    uint32_t frameCount = 0;
} s_presentationOverride;

#if TARGET_PC

// ── Capture buffers ──────────────────────────────────────────────────────────
// MAX_LOCAL_VIEWS capture slots.  Slot 0 aliases the main framebuffer texture
// (m_fullFrameBufferTex).  Slots 1..N-1 are heap-allocated on demand.

struct CaptureSlot {
    ResTIMG* timg = nullptr;
    void* tex = nullptr;
    TGXTexObj texObj{};
    bool captured = false;
    bool heapAllocated = false;
};
static std::array<CaptureSlot, MAX_LOCAL_VIEWS> s_captureSlots;

static void freeCaptureSlot(CaptureSlot& slot) {
    if (slot.timg != nullptr) {
        if (slot.heapAllocated) {
            std::free(slot.timg);  // allocated via aligned_alloc
        } else {
            // JKR-heap allocated: find the owning heap and free through it.
            JKRHeap* heap = JKRHeap::findFromRoot(slot.timg);
            if (heap != nullptr) {
                heap->free(slot.timg);
            }
        }
    }
    slot.timg = nullptr;
    slot.tex = nullptr;
    std::memset(&slot.texObj, 0, sizeof(slot.texObj));
    slot.captured = false;
    slot.heapAllocated = false;
}

static bool ensureCaptureSlot(ViewId view) {
    if (view >= MAX_LOCAL_VIEWS) {
        return false;
    }
    // Slot 0 uses the main framebuffer texture — nothing to allocate.
    if (view == 0) {
        if (mDoGph_gInf_c::m_fullFrameBufferTimg == nullptr ||
            mDoGph_gInf_c::m_fullFrameBufferTex == nullptr) {
            return false;
        }
        return true;
    }

    CaptureSlot& slot = s_captureSlots[view];
    if (slot.timg != nullptr && slot.tex != nullptr) {
        return true;
    }

    if (mDoGph_gInf_c::m_fullFrameBufferTimg == nullptr) {
        return false;
    }

    const u16 w = mDoGph_gInf_c::m_fullFrameBufferTimg->width;
    const u16 h = mDoGph_gInf_c::m_fullFrameBufferTimg->height;
    const u32 format = mDoGph_gInf_c::m_fullFrameBufferTimg->format;
    const u32 rawSize = GXGetTexBufferSize(w, h, format, GX_FALSE, 0) + 0x20;
    const u32 bufferSize = (rawSize + 0x1fu) & ~0x1fu;

    void* mem = std::aligned_alloc(0x20, bufferSize);
    if (mem != nullptr) {
        slot.heapAllocated = true;
    }
    if (mem == nullptr) {
        debug::logError("multi-view capture: failed to allocate slot %u buffer (%u bytes)",
                        view, bufferSize);
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
    slot.timg = timg;
    slot.tex = reinterpret_cast<char*>(timg) + sizeof(ResTIMG);
    debug::logInfo("multi-view capture: slot %u buffer ready (%u bytes)", view, bufferSize);
    return true;
}

// ── Grid layout ──────────────────────────────────────────────────────────────
static void gridDimensions(uint8_t viewCount, uint8_t& cols, uint8_t& rows) {
    if (viewCount <= 1) {
        cols = 1;
        rows = 1;
    } else if (viewCount <= 2) {
        cols = 2;
        rows = 1;
    } else if (viewCount <= 4) {
        cols = 2;
        rows = 2;
    } else if (viewCount <= 6) {
        cols = 3;
        rows = 2;
    } else {
        cols = 4;
        rows = 2;
    }
}

static void setupPresentGXState() {
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
    // The captured framebuffer uses the normal screen orientation.  Keep the
    // horizontal bounds in ascending order; reversing them mirrors every pane.
    MTXOrtho(ortho, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 10.0f);
    GXSetProjection(ortho, GX_ORTHOGRAPHIC);
    GXLoadPosMtxImm(cMtx_getIdentity(), GX_PNMTX0);
    GXSetCurrentMtx(0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_RGB8, 0);
}

static void blitPane(f32 x, f32 y, f32 w, f32 h) {
    GXSetViewport(x, y, w, h, 0.0f, 1.0f);
    GXSetScissor(static_cast<u32>(x), static_cast<u32>(y), static_cast<u32>(w),
                 static_cast<u32>(h));
    mDoGph_drawFilterQuad(1, 1);
}

#endif  // TARGET_PC

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

    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        fopAc_ac_c* player = dComIfGp_getPlayer(id);
        if (player == nullptr) {
            continue;
        }
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

// ── ScopedWorldDrawPass ──────────────────────────────────────────────────────

ScopedWorldDrawPass::ScopedWorldDrawPass(ViewId view) : context_(makeFrame(view)) {

    prevWindow_ = g_dComIfG_gameInfo.play.mCurrentWindow;
    prevPlayView_ = g_dComIfG_gameInfo.play.mCurrentView;
    prevPlayViewport_ = g_dComIfG_gameInfo.play.mCurrentViewport;
    prevDrawView_ = dComIfGd_getView();
    prevDrawViewport_ = dComIfGd_getViewport();

    dDlst_window_c* window = resolveWindow(view);
    camera_process_class* camera = resolveCamera(view);

    if (window != nullptr) {
        dComIfGp_setCurrentWindow(window);
        dComIfGd_setWindow(window);
    }
    if (camera != nullptr) {
        dComIfGp_setCurrentView(&camera->view);
        dComIfGd_setView(&camera->view);
    }

    // Full-frame viewport for capture-based rendering.
    // The window viewport was set to full-FB by beginMultiViewCapture().
    if (window != nullptr && isMultiViewActive()) {
        view_port_class* port = window->getViewPort();
        if (port != nullptr) {
            dComIfGp_setCurrentViewport(port);
            dComIfGd_setViewport(port);
            GXSetViewport(port->x_orig, port->y_orig, port->width, port->height,
                          port->near_z, port->far_z);
            GXSetScissor(static_cast<u32>(port->scissor.x_orig),
                         static_cast<u32>(port->scissor.y_orig),
                         static_cast<u32>(port->scissor.width),
                         static_cast<u32>(port->scissor.height));
        }
    }

    // Per-view senses visualization.
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

// ── Lifecycle ────────────────────────────────────────────────────────────────

void init() {
#if TARGET_PC
    for (auto& slot : s_captureSlots) {
        slot.captured = false;
    }
#endif
    g_lastHash = 0;
    g_simTicks = 0;
    g_lastPassCount = 1;
    s_presentationOverride = {};
}

void reset() {
    releaseAllCaptureSlots();
    init();
}

void beginFrame() {}

void endFrame() {
    g_lastHash = computeFrameStateHash();
    // Track frame count while presentation override is active (diagnostics).
    if (s_presentationOverride.active) {
        ++s_presentationOverride.frameCount;
    }
}

void noteSimulationTick() {
    ++g_simTicks;
}

// ── View counts and lookups ──────────────────────────────────────────────────

uint8_t worldDrawPassCount() {
#if TARGET_PC
    // Presentation override: initiator-owned conversations always render
    // full-screen from the initiator's camera.  Activate on push, not only
    // after dComIfGp_event_runCheck(), to prevent split-screen grid rendering
    // between the event order and the first event-running frame.
    if (s_presentationOverride.active) {
        return 1;
    }

    // Event cameras/cutscenes are authored for the vanilla single-view
    // pipeline. Rendering them once also avoids duplicating demo actors and
    // fullscreen effects into the split-screen capture grid.
    if (dComIfGp_event_runCheck() && !event::hasWaitingForPartyEvent()) {
        return 1;
    }
#endif

    const uint8_t n = runtime().activeViewCount;
    return n < 1 ? 1 : n;
}

bool isMultiViewActive() {
    return worldDrawPassCount() > 1;
}

dDlst_window_c* resolveWindow(ViewId view) {
    if (s_presentationOverride.active || !isMultiViewActive()) {
        (void)view;
        return dComIfGp_getWindow(0);
    }
    COOP_ASSERT(view < MAX_LOCAL_VIEWS);
    return dComIfGp_getWindow(view);
}

camera_process_class* resolveCamera(ViewId view) {
    // Presentation override: use the initiator's camera instead of camera 0.
    // This ensures the event's full-screen pass uses the right point-of-view
    // during initiator-owned conversations.
    if (s_presentationOverride.active) {
        camera_class* initiatorCam = getCameraProcess(s_presentationOverride.owner);
        if (initiatorCam != nullptr) {
            return reinterpret_cast<camera_process_class*>(initiatorCam);
        }
        // Secondary camera may be null if it hasn't finished async creation yet.
        // When worldDrawPassCount() returns 1 during override, view is always 0,
        // but guard against a stale override with a null owner camera anyway.
        debug::logWarn(
            "resolveCamera: presentation override active but owner=view%u "
            "camera null, falling back to camera 0",
            static_cast<unsigned>(s_presentationOverride.owner));
        // Fall through to camera 0 rather than entering the multi-view branch.
        return dComIfGp_getCamera(0);
    }
    if (isMultiViewActive()) {
        if (camera_class* cam = getCameraProcess(view)) {
            return reinterpret_cast<camera_process_class*>(cam);
        }
    }
    return dComIfGp_getCamera(0);
}

// ── Multi-view capture / present ─────────────────────────────────────────────

void beginMultiViewCapture() {
#if !TARGET_PC
    return;
#else
    const uint8_t count = worldDrawPassCount();
    if (count <= 1) {
        return;
    }

    g_lastPassCount = count;
    const f32 fbW = static_cast<f32>(FB_WIDTH);
    const f32 fbH = static_cast<f32>(FB_HEIGHT);

    // Set every active window to full-frame viewport.  Each view renders
    // full-frame for capture — no tiled scissors.
    // NOTE: scissor is intentionally NOT overridden here — widezoom_correction()
    // already set it per-camera during camera_execute to reflect each camera's
    // trim height (lock-on black bars / letterbox).  Preserving it lets
    // trimming() draw the correct per-view black bars later.
    for (ViewId v = 0; v < count; ++v) {
        dDlst_window_c* window = dComIfGp_getWindow(v);
        if (window == nullptr) {
            continue;
        }
        window->setViewPort(0.0f, 0.0f, fbW, fbH, 0.0f, 1.0f);
        window->setCameraID(static_cast<int>(v));
        window->setMode(2);
    }

    // Ensure capture buffers for all views.
    for (ViewId v = 0; v < count; ++v) {
        if (!ensureCaptureSlot(v)) {
            debug::logError("beginMultiViewCapture: failed to ensure slot %u", v);
        }
        s_captureSlots[v].captured = false;
    }
#endif
}

void captureView(ViewId view) {
#if !TARGET_PC
    (void)view;
#else
    const uint8_t count = worldDrawPassCount();
    if (count <= 1) {
        return;
    }
    if (view >= MAX_LOCAL_VIEWS) {
        return;
    }
    if (mDoGph_gInf_c::m_fullFrameBufferTimg == nullptr ||
        mDoGph_gInf_c::m_fullFrameBufferTex == nullptr) {
        return;
    }

    CaptureSlot& slot = s_captureSlots[view];
    void* dest = nullptr;
    if (view == 0) {
        dest = mDoGph_gInf_c::m_fullFrameBufferTex;
    } else {
        if (slot.tex == nullptr) {
            return;
        }
        dest = slot.tex;
    }

    const f32 fbW = mDoGph_gInf_c::getWidth();
    const f32 fbH = mDoGph_gInf_c::getHeight();
    if (fbW < 2.0f || fbH < 1.0f) {
        return;
    }

    // Clear EFB after capturing view 0 so view 1 starts clean.
    const bool clearAfter = (view == 0);
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

    slot.captured = true;
#endif
}

void presentMultiViewGrid() {
#if !TARGET_PC
    return;
#else
    const uint8_t count = worldDrawPassCount();
    if (count <= 1) {
        return;
    }

    // Check all slots captured before presenting.
    for (ViewId v = 0; v < count; ++v) {
        if (!s_captureSlots[v].captured) {
            return;
        }
    }

    const f32 fbW = mDoGph_gInf_c::getWidth();
    const f32 fbH = mDoGph_gInf_c::getHeight();
    if (fbW < 2.0f || fbH < 1.0f) {
        return;
    }

    uint8_t cols, rows;
    gridDimensions(count, cols, rows);
    const f32 cellW = fbW / static_cast<f32>(cols);
    const f32 cellH = fbH / static_cast<f32>(rows);

    setupPresentGXState();

    for (ViewId v = 0; v < count; ++v) {
        CaptureSlot& slot = s_captureSlots[v];
        ResTIMG* timg = (v == 0) ? mDoGph_gInf_c::m_fullFrameBufferTimg : slot.timg;
        if (timg == nullptr) {
            continue;
        }

        TGXTexObj* texObj;
        if (v == 0) {
            texObj = &mDoGph_gInf_c::m_fullFrameBufferTexObj;
        } else {
            texObj = &slot.texObj;
        }

        mDoLib_setResTimgObj(timg, texObj, 0, nullptr);
        GXLoadTexObj(texObj, GX_TEXMAP0);

        const uint8_t col = v % cols;
        const uint8_t row = v / cols;
        const f32 x = cellW * static_cast<f32>(col);
        const f32 y = cellH * static_cast<f32>(row);
        blitPane(x, y, cellW, cellH);
    }

    // Restore full FB for subsequent passes.
    GXSetViewport(0.0f, 0.0f, fbW, fbH, 0.0f, 1.0f);
    GXSetScissor(0, 0, static_cast<u32>(fbW), static_cast<u32>(fbH));
    j3dSys.reinitGX();

    // Mark all slots consumed.
    for (auto& slot : s_captureSlots) {
        slot.captured = false;
    }
#endif
}

ViewportRect gridCellViewport(ViewId view) {
    ViewportRect rect;
    const uint8_t count = worldDrawPassCount();
    if (count <= 1) {
        return rect;  // {0,0,1,1}
    }

    uint8_t cols, rows;
    gridDimensions(count, cols, rows);
    const uint8_t col = view % cols;
    const uint8_t row = view / cols;
    rect.width = 1.0f / static_cast<f32>(cols);
    rect.height = 1.0f / static_cast<f32>(rows);
    rect.x = rect.width * static_cast<f32>(col);
    rect.y = rect.height * static_cast<f32>(row);
    return rect;
}

f32 paneAspect() {
    const f32 full = mDoGph_gInf_c::getAspect();
    const uint8_t count = worldDrawPassCount();
    if (count <= 1) {
        return full;
    }
    uint8_t cols, rows;
    gridDimensions(count, cols, rows);
    // Each grid cell has aspect = (fbW/cols) / (fbH/rows) = full * (rows/cols).
    return full * (static_cast<f32>(rows) / static_cast<f32>(cols));
}

void bindPainterCameraView(ViewId view, camera_process_class* camera) {
#if !TARGET_PC
    (void)view;
    (void)camera;
#else
    if (camera == nullptr) {
        return;
    }

    // Rebuild proj/view matrices for this painter pass using the pane aspect.
    const f32 aspect = isMultiViewActive() ? paneAspect() : camera->view.aspect;
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

void releaseCaptureSlot(ViewId view) {
#if TARGET_PC
    if (view >= MAX_LOCAL_VIEWS) {
        return;
    }
    freeCaptureSlot(s_captureSlots[view]);
#else
    (void)view;
#endif
}

void releaseAllCaptureSlots() {
#if TARGET_PC
    for (auto& slot : s_captureSlots) {
        freeCaptureSlot(slot);
    }
#endif
}

// ── Presentation override ────────────────────────────────────────────────────

void pushConversationPresentation(ViewId owner) {
    if (owner >= MAX_LOCAL_VIEWS) {
        debug::logWarn(
            "pushConversationPresentation: invalid owner=%u, ignoring",
            static_cast<unsigned>(owner));
        return;
    }
    if (s_presentationOverride.active) {
        if (s_presentationOverride.owner == owner) {
            debug::logInfo(
                "pushConversationPresentation: already active owner=view%u "
                "frame=%u — ignoring duplicate push",
                static_cast<unsigned>(owner),
                static_cast<unsigned>(s_presentationOverride.frameCount));
            return;
        }
        // Different owner — log and replace.  This shouldn't normally happen
        // because conversations are deduplicated, but handle it gracefully.
        debug::logWarn(
            "pushConversationPresentation: replacing owner view%u -> view%u "
            "frame=%u",
            static_cast<unsigned>(s_presentationOverride.owner),
            static_cast<unsigned>(owner),
            static_cast<unsigned>(s_presentationOverride.frameCount));
    }

    // Save the current multi-view layout BEFORE changing it.
    s_presentationOverride.savedViewCount = runtime().activeViewCount;
    s_presentationOverride.owner = owner;
    s_presentationOverride.active = true;
    s_presentationOverride.frameCount = 0;

    debug::logInfo(
        "presentation: push owner=view%u savedViewCount=%u",
        static_cast<unsigned>(owner),
        static_cast<unsigned>(s_presentationOverride.savedViewCount));
}

void popConversationPresentation() {
    if (!s_presentationOverride.active) {
        debug::logInfo(
            "popConversationPresentation: no active override — nothing to pop");
        return;
    }

    const uint8_t savedCount = s_presentationOverride.savedViewCount;
    const ViewId owner = s_presentationOverride.owner;
    const uint32_t duration = s_presentationOverride.frameCount;

    s_presentationOverride = {};

    // Restore the saved activeViewCount.  If the saved count is stale (e.g.,
    // because players joined/left during the event), clamp to current state.
    uint8_t restoreCount = savedCount;
    if (restoreCount < 1) restoreCount = 1;
    if (restoreCount > MAX_LOCAL_VIEWS) restoreCount = MAX_LOCAL_VIEWS;
    runtime().activeViewCount = restoreCount;

    // Defense-in-depth: if the saved count was already 1 and the override
    // was somehow pushed during a single-view scenario, the restore is a
    // no-op but we log it for diagnostics.
    if (savedCount != restoreCount) {
        debug::logInfo(
            "presentation: pop restored count clamped %u -> %u",
            static_cast<unsigned>(savedCount),
            static_cast<unsigned>(restoreCount));
    }

    debug::logInfo(
        "presentation: pop owner=view%u duration=%u frames restoredViewCount=%u",
        static_cast<unsigned>(owner),
        static_cast<unsigned>(duration),
        static_cast<unsigned>(restoreCount));
}

bool isConversationPresentationActive() {
    return s_presentationOverride.active;
}

ViewId getConversationPresentationOwner() {
    return s_presentationOverride.active
               ? s_presentationOverride.owner
               : static_cast<ViewId>(0);
}

uint32_t presentationOverrideFrameCount() {
    return s_presentationOverride.frameCount;
}

}  // namespace dusk::coop::render
