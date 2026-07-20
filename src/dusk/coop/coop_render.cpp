#include "dusk/coop/coop_render.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_accessors.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_forms.h"

#include "SSystem/SComponent/c_counter.h"
#include "d/d_com_inf_game.h"
#include "d/d_drawlist.h"
#include "d/d_kankyo.h"
#include "f_op/f_op_actor.h"
#include "f_op/f_op_camera_mng.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_graphic.h"

#include <array>
#include <cstring>

namespace dusk::coop::render {
namespace {

bool g_effectsDisabled = true;
uint64_t g_lastHash = 0;
uint32_t g_simTicks = 0;
uint32_t g_lastPassCount = 1;
uint8_t g_forcedViewCount = 0;

std::array<dDlst_window_c, MAX_LOCAL_VIEWS> g_windowSidecar{};
bool g_savedPrimaryViewport = false;
view_port_class g_primaryViewportBackup{};

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
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    {fpcNm_COOP_PROXY_e, ActorDrawClass::Independent},
#endif
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

    for (ViewId v = 0; v < count; ++v) {
        const ViewportRect rect = viewportFor(v, count, mode);
        dDlst_window_c* window = (v == 0) ? dComIfGp_getWindow(0) : &g_windowSidecar[v];
        applyViewportToWindow(window, rect, fbW, fbH);
        // Same-camera PoC: every sidecar points at camera slot 0 (never index >0).
        window->setCameraID(0);
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
    g_effectsDisabled = true;
    g_lastHash = 0;
    g_simTicks = 0;
    g_lastPassCount = 1;
    g_forcedViewCount = 0;
    g_windowSidecar = {};
    g_primaryViewportBackup = {};
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
    // Gate A same-camera PoC: always the original camera slot 0 unless Gate B registered one.
    if (isEnabled() && view > 0) {
        if (camera_class* secondary = getCameraProcess(view)) {
            return reinterpret_cast<camera_process_class*>(secondary);
        }
    }
    return dComIfGp_getCamera(0);
}

ViewportRect viewportFor(ViewId view, uint8_t activeViewCount, ViewAssignmentMode mode) {
    ViewportRect rect;
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
    if (isEnabled() && count > 1) {
        runtime().activeViewCount = count;
    }
}

uint8_t forcedViewCount() { return g_forcedViewCount; }

}  // namespace dusk::coop::render
