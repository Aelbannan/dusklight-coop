#pragma once

#include "dusk/coop/coop_context.h"
#include "dusk/coop/coop_types.h"

class dDlst_window_c;
class camera_process_class;
struct view_class;
struct view_port_class;

namespace dusk::coop::render {

struct ViewportRect {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 width = 1.0f;
    f32 height = 1.0f;
};

enum class ActorDrawClass : uint8_t {
    Independent,
    DependentPure,
    DependentMutating,
};

enum class IncompatibleEffect : uint8_t {
    MotionBlur,
    DepthOfField,
    FrameBufferCapture,
    Bloom,
    FullFrameFade,
    MirrorModeCopy,
    ScreenSpaceParticles,
};

// RAII: save/restore current window/view/viewport pointers + co-op context.
class ScopedWorldDrawPass {
public:
    explicit ScopedWorldDrawPass(ViewId view);
    ~ScopedWorldDrawPass();

    ScopedWorldDrawPass(const ScopedWorldDrawPass&) = delete;
    ScopedWorldDrawPass& operator=(const ScopedWorldDrawPass&) = delete;

private:
    ScopedContext context_;
    dDlst_window_c* prevWindow_ = nullptr;
    view_class* prevPlayView_ = nullptr;
    view_port_class* prevPlayViewport_ = nullptr;
    view_class* prevDrawView_ = nullptr;
    view_port_class* prevDrawViewport_ = nullptr;
    // Gate I: per-view senses visualization override.
    u8 prevSensesEffect_ = 0;
    f32 prevSensesStrength_ = 0.0f;
    bool sensesOverride_ = false;
    bool active_ = false;
};

void init();
void reset();

// Gate A: replay world draw for each active view without advancing simulation.
void beginFrame();
void endFrame();

// Called once when dScnPly_Draw advances simulation counters (not per view).
void noteSimulationTick();

// Sync sidecar viewports; returns true when painter must loop (2+ views).
bool drawViews();

uint8_t worldDrawPassCount();
bool isMultiViewActive();

// Window/camera resolution for a painter pass. Never indexes original arrays with >0.
dDlst_window_c* resolveWindow(ViewId view);
camera_process_class* resolveCamera(ViewId view);

ViewportRect viewportFor(ViewId view, uint8_t activeViewCount, ViewAssignmentMode mode);
void applyViewportToWindow(dDlst_window_c* window, const ViewportRect& norm, f32 fbWidth,
                           f32 fbHeight);

ActorDrawClass classifyActor(s16 procName);

uint64_t lastFrameStateHash();
uint32_t simulationTickCounter();
uint32_t lastWorldDrawPassCount();

void setIncompatibleEffectsDisabled(bool disabled);
bool incompatibleEffectsDisabled();
bool shouldSkipEffect(IncompatibleEffect effect);

// PoC helper: force N same-camera tiled views while co-op is enabled (does not create cameras).
void setForcedViewCount(uint8_t count);
uint8_t forcedViewCount();

}  // namespace dusk::coop::render
