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

    // Convert to pixel coordinates.
    const f32 fbW = mDoGph_gInf_c::getWidthF();
    const f32 fbH = mDoGph_gInf_c::getHeightF();
    const f32 vpX = vpRect.x * fbW;
    const f32 vpY = vpRect.y * fbH;
    const f32 vpW = vpRect.width * fbW;
    const f32 vpH = vpRect.height * fbH;

    if (vpW < 1.0f || vpH < 1.0f) {
        return;
    }

    // Uniform scale so the full-FB HUD fits without distortion.
    const f32 scale = std::min(vpW / fbW, vpH / fbH);

    // --- Set up a viewport-sized 2D ortho graph ---
    J2DGrafContext* prevGraf = dComIfGp_getCurrentGrafPort();

    J2DOrthoGraph viewportOrtho(vpX, vpY, vpW, vpH, -1.0f, 1.0f);
    viewportOrtho.setPort();
    dComIfGp_setCurrentGrafPort(&viewportOrtho);

    // --- Allow the vanilla HUD draw to execute ---
    const bool prevActive = g_perViewHudActive;
    g_perViewHudActive = false;

    // Scale the root pane so HUD content fits the viewport proportionally.
    CPaneMgr* rootPane = g_hudDraw->getRootPane();
    if (rootPane != nullptr) {
        const f32 origScaleX = rootPane->getScaleX();
        const f32 origScaleY = rootPane->getScaleY();
        const f32 origTransX = rootPane->getTranslateX();
        const f32 origTransY = rootPane->getTranslateY();

        rootPane->scale(scale, scale);
        rootPane->paneTrans(0.0f, 0.0f);

        g_hudDraw->draw();

        rootPane->scale(origScaleX, origScaleY);
        rootPane->paneTrans(origTransX, origTransY);
    } else {
        g_hudDraw->draw();
    }

    g_perViewHudActive = prevActive;

    // Restore the previous graf port.
    dComIfGp_setCurrentGrafPort(reinterpret_cast<J2DOrthoGraph*>(prevGraf));
#endif
}

}  // namespace dusk::coop::hud
