#pragma once

#include "dusk/coop/coop_context.h"
#include "dusk/coop/coop_types.h"

class dDlst_window_c;
class camera_process_class;
struct view_class;
struct view_port_class;

namespace dusk::coop::render {

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
    // Per-view senses visualization override.
    u8 prevSensesEffect_ = 0;
    f32 prevSensesStrength_ = 0.0f;
    bool sensesOverride_ = false;
    bool active_ = false;
};

void init();
void reset();

void beginFrame();
void endFrame();
void noteSimulationTick();

uint8_t worldDrawPassCount();
bool isMultiViewActive();

// Window/camera resolution for a painter pass.
dDlst_window_c* resolveWindow(ViewId view);
camera_process_class* resolveCamera(ViewId view);

// Set up native windows for full-frame multi-view capture.
void beginMultiViewCapture();

// Capture the current EFB for the given view into its capture buffer.
void captureView(ViewId view);

// Blit all captured views into an N-up grid on screen.
void presentMultiViewGrid();

// Normalized viewport rectangle for one grid cell (for HUD positioning).
// Returns {0,0,1,1} when only one view is active.
struct ViewportRect {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 width = 1.0f;
    f32 height = 1.0f;
};
ViewportRect gridCellViewport(ViewId view);

// Aspect ratio for each pane in the grid, used for camera projection.
f32 paneAspect();

// Before rasterizing a painter pass, rebuild camera view/proj matrices
// for the pane aspect.
void bindPainterCameraView(ViewId view, camera_process_class* camera);

}  // namespace dusk::coop::render
