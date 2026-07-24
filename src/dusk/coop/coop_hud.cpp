#include "dusk/coop/coop_hud.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_context.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_inventory.h"
#include "dusk/coop/coop_render.h"
#include "dusk/coop/coop_camera.h"

#include "JSystem/J2DGraph/J2DOrthoGraph.h"
#include "JSystem/J2DGraph/J2DScreen.h"
#include "JSystem/J2DGraph/J2DGrafContext.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#include "d/d_meter2_draw.h"
#include "d/d_meter2.h"
#include "d/d_meter_HIO.h"
#include "m_Do/m_Do_graphic.h"
#include "d/d_pane_class.h"

#include <algorithm>
#include <cmath>

namespace dusk::coop::hud {

bool g_perViewHudActive = false;

namespace {

// Reference to the vanilla HUD draw object (found once after it's created).
dMeter2Draw_c* g_hudDraw = nullptr;

// Full framebuffer dimensions used for layout calculations.
constexpr f32 FB_W = 608.0f;
constexpr f32 FB_H = 456.0f;

}  // namespace

void init() {
    g_perViewHudActive = false;
    g_hudDraw = nullptr;
}

void reset() {
    init();
}

void tick() {
#if !TARGET_PC
    return;
#else
    // Always use the per-view HUD path for all views (single and multi).
    // The single-view case is just a full-FB viewport with uniform scale 1.0.
    g_perViewHudActive = true;

    // Locate the vanilla HUD draw object once.
    if (g_hudDraw == nullptr) {
        dMeter2_c* meter = dMeter2Info_getMeterClass();
        if (meter != nullptr) {
            g_hudDraw = meter->getMeterDrawPtr();
        }
    }

    // If the HUD object isn't available yet, don't suppress the global draw.
    if (g_hudDraw == nullptr) {
        g_perViewHudActive = false;
    }
#endif
}

void drawView(ViewId view) {
#if !TARGET_PC
    (void)view;
    return;
#else
    if (!g_perViewHudActive) {
        return;
    }
    if (g_hudDraw == nullptr) {
        return;
    }

    // Get the viewport dimensions from the current window.
    dDlst_window_c* window = render::resolveWindow(view);
    if (window == nullptr) {
        return;
    }
    view_port_class* vp = window->getViewPort();
    if (vp == nullptr) {
        return;
    }

    const f32 vpX = vp->x_orig;
    const f32 vpY = vp->y_orig;
    const f32 vpW = vp->width;
    const f32 vpH = vp->height;

    if (vpW < 1.0f || vpH < 1.0f) {
        return;
    }

    // Uniform scale so the full-FB HUD fits without distortion.
    const f32 scale = std::min(vpW / FB_W, vpH / FB_H);

    // Save the current graf port and override with a viewport-sized ortho.
    // The getter returns J2DGrafContext*, but the setter expects J2DOrthoGraph*.
    J2DGrafContext* prevGraf = dComIfGp_getCurrentGrafPort();

    // Set up a viewport-sized 2D ortho graph.
    // Drawing the J2DScreen at (0,0) in this ortho places it at the viewport origin.
    J2DOrthoGraph viewportOrtho(vpX, vpY, vpW, vpH, -1.0f, 1.0f);
    viewportOrtho.setPort();
    dComIfGp_setCurrentGrafPort(&viewportOrtho);

    // Temporarily allow per-view draw (suppress the global-draw skip check).
    const bool prevActive = g_perViewHudActive;
    g_perViewHudActive = false;

    // Apply uniform scaling to the root pane so all HUD elements fit the viewport.
    CPaneMgr* rootPane = g_hudDraw->getRootPane();
    if (rootPane != nullptr) {
        const f32 origScaleX = rootPane->getScaleX();
        const f32 origScaleY = rootPane->getScaleY();
        const f32 origTransX = rootPane->getTranslateX();
        const f32 origTransY = rootPane->getTranslateY();

        // Scale uniformly and position at the viewport origin.
        rootPane->scale(scale, scale);
        rootPane->paneTrans(0.0f, 0.0f);

        // Draw the vanilla HUD. It reads per-player data through
        // dComIfGs_* → dusk_coop_get* → currentPlayer() (set by the active ScopedContext).
        g_hudDraw->draw();

        // Restore root pane so the next viewport draw starts clean.
        rootPane->scale(origScaleX, origScaleY);
        rootPane->paneTrans(origTransX, origTransY);
    } else {
        g_hudDraw->draw();
    }

    // Restore the flag for the global-draw skip check.
    g_perViewHudActive = prevActive;

    // Restore the previous graf port.
    dComIfGp_setCurrentGrafPort(reinterpret_cast<J2DOrthoGraph*>(prevGraf));
#endif
}

}  // namespace dusk::coop::hud
