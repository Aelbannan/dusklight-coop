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
    g_perViewHudActive = true;

    // Refresh the HUD draw pointer every frame — the dMeter2_c process can get
    // destroyed and recreated during scene transitions / camera init when a new
    // player joins.  The old pointer would dangle and calling draw() on it is UB.
    dMeter2_c* meter = dMeter2Info_getMeterClass();
    if (meter != nullptr) {
        g_hudDraw = meter->getMeterDrawPtr();
    }

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
    if (!g_perViewHudActive || g_hudDraw == nullptr) {
        return;
    }

    // --- Compute the viewport rectangle for this view ---
    render::ViewportRect vpRect = render::gridCellViewport(view);

    // Convert to pixel coordinates in the internal FB coordinate space.
    const f32 fbW = mDoGph_gInf_c::getWidthF();
    const f32 fbH = mDoGph_gInf_c::getHeightF();
    const f32 vpX = vpRect.x * fbW;
    const f32 vpY = vpRect.y * fbH;
    const f32 vpW = vpRect.width * fbW;
    const f32 vpH = vpRect.height * fbH;

    if (vpW < 1.0f || vpH < 1.0f) {
        return;
    }

    // --- Set up a 2D ortho graph that maps the full FB coordinate space
    //     into the viewport.  HUD elements are positioned in FB coordinates
    //     (e.g. mLifeGaugePosX = 5).  Without root-pane scaling, those
    //     positions stay correct and dAnchorHudScale shifts remain consistent
    //     regardless of viewport width.
    J2DGrafContext* prevGraf = dComIfGp_getCurrentGrafPort();

    // The ortho bounds cover the full FB space (0,0)-(fbW,fbH), and the
    // viewport is set to the grid cell.  The GX backend maps from FB
    // coordinates to display pixels, so an element at FB position (5,18)
    // appears at the same physical location in both full-screen and
    // split-screen modes.
    J2DOrthoGraph viewportOrtho(vpX, vpY, vpW, vpH, -1.0f, 1.0f);
    // Override the ortho bounds to cover the full FB so layout coords map
    // directly without scaling the pane hierarchy.
    viewportOrtho.setOrtho(0.0f, 0.0f, fbW, fbH, -1.0f, 1.0f);
    viewportOrtho.setPort();
    dComIfGp_setCurrentGrafPort(&viewportOrtho);

    // --- Allow the vanilla HUD draw to execute ---
    const bool prevActive = g_perViewHudActive;
    g_perViewHudActive = false;

    g_hudDraw->draw();

    g_perViewHudActive = prevActive;

    // Restore the previous graf port.
    dComIfGp_setCurrentGrafPort(reinterpret_cast<J2DOrthoGraph*>(prevGraf));
#endif
}

}  // namespace dusk::coop::hud
