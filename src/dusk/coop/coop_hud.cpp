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
#include "m_Do/m_Do_graphic.h"

namespace dusk::coop::hud {

bool g_perViewHudActive = false;
PlayerHudButtonState g_playerButtonState[MAX_LOCAL_PLAYERS]{};
s16 g_patchPlayerForDraw = -1;

namespace {

dMeter2Draw_c* g_hudDraw = nullptr;

}  // namespace

void init() {
    g_perViewHudActive = false;
    g_hudDraw = nullptr;
    g_patchPlayerForDraw = -1;
    for (auto& s : g_playerButtonState) {
        s = PlayerHudButtonState{};
    }
}

void reset() {
    init();
}

void tick() {
#if !TARGET_PC
    return;
#else
    g_perViewHudActive = true;

    dMeter2_c* meter = dMeter2Info_getMeterClass();
    if (meter != nullptr) {
        g_hudDraw = meter->getMeterDrawPtr();
    }

    if (g_hudDraw == nullptr) {
        g_perViewHudActive = false;
    }

    // Push loadout items (X/Y slots) into per-player button state.
    for (PlayerId p = 0; p < MAX_LOCAL_PLAYERS; ++p) {
        if (!isValidPlayer(p) || !playerSlot(p)->joined) {
            continue;
        }
        auto* pr = playerRuntime(p);
        if (pr == nullptr) continue;
        auto& btn = g_playerButtonState[p];
        btn.itemSlotX = pr->loadout.itemX;
        btn.itemSlotY = pr->loadout.itemY;
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

    const PlayerId player = currentPlayer();

    // Viewport rect for this view.
    const render::ViewportRect vpRect = render::gridCellViewport(view);

    const f32 fbW = mDoGph_gInf_c::getWidthF();
    const f32 fbH = mDoGph_gInf_c::getHeightF();
    const f32 vpX = vpRect.x * fbW;
    const f32 vpY = vpRect.y * fbH;
    const f32 vpW = vpRect.width * fbW;
    const f32 vpH = vpRect.height * fbH;

    if (vpW < 1.0f || vpH < 1.0f) {
        return;
    }

    J2DGrafContext* prevGraf = dComIfGp_getCurrentGrafPort();

    J2DOrthoGraph viewportOrtho(vpX, vpY, vpW, vpH, -1.0f, 1.0f);
    viewportOrtho.setOrtho(0.0f, 0.0f, fbW, fbH, -1.0f, 1.0f);
    viewportOrtho.setPort();
    dComIfGp_setCurrentGrafPort(&viewportOrtho);

    // Signal dMeter2Draw_c::draw() to patch button-text panes for this
    // player before rendering the normal HUD content.
    g_patchPlayerForDraw = static_cast<s16>(player);

    const bool prevActive = g_perViewHudActive;
    g_perViewHudActive = false;

    g_hudDraw->draw();

    g_perViewHudActive = prevActive;
    g_patchPlayerForDraw = -1;

    dComIfGp_setCurrentGrafPort(reinterpret_cast<J2DOrthoGraph*>(prevGraf));
#endif
}

}  // namespace dusk::coop::hud
