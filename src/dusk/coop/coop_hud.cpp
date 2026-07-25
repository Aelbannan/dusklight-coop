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
#include "JSystem/JKernel/JKRHeap.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#include "d/d_meter2_draw.h"
#include "d/d_meter2.h"
#include "d/d_meter_map.h"
#include "m_Do/m_Do_graphic.h"
#include "d/d_meter_HIO.h"
#include "d/d_meter_button.h"
#include "d/d_menu_window.h"

// g_playerEmpButton defined at global scope to match the extern declaration.
dMeterButton_c* g_playerEmpButton[8]{};

// Per-view emphasis button instances — indexed by ViewId.
dMeterButton_c* g_viewEmpButton[8]{};

namespace dusk::coop::hud {

PlayerHudButtonState g_playerButtonState[MAX_LOCAL_PLAYERS]{};
HudAnimState g_hudAnim[MAX_LOCAL_PLAYERS]{};
s16 g_patchPlayerForDraw = -1;

namespace {

::dMeter2Draw_c* g_hudDraw = nullptr;

}  // namespace

void init() {
    g_hudDraw = nullptr;
    g_patchPlayerForDraw = -1;
    for (auto& s : g_playerButtonState) {
        s = PlayerHudButtonState{};
    }
    for (auto& a : g_hudAnim) {
        a = HudAnimState{};
    }
    for (auto& e : g_playerEmpButton) {
        e = nullptr;
    }
    for (auto& e : g_viewEmpButton) {
        e = nullptr;
    }
}

void reset() {
    for (auto& e : g_playerEmpButton) {
        if (e != nullptr) {
            dusk_coop_destroyEmpButton(&e);
        }
    }
    for (auto& e : g_viewEmpButton) {
        if (e != nullptr) {
            dusk_coop_destroyEmpButton(&e);
        }
    }
    init();
}

void tick() {
#if !TARGET_PC
    return;
#else
    dMeter2_c* meter = dMeter2Info_getMeterClass();
    if (meter != nullptr) {
        g_hudDraw = meter->getMeterDrawPtr();
    }

    if (g_hudDraw == nullptr) {
        return;
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

    // Emphasis button processing is now done per-view in drawView(),
    // using currentPlayer() to pick the correct player state.
    // This avoids cross-player contamination from the tick-time loop.

    // Track item-wheel player affinity. When the window status transitions
    // to 2 (ring open), set the ring player.  For now the first-joined
    // player is the default; per-player input dispatch can override this
    // by calling dMw_setRingPlayer() before the wheel opens.
    {
        static u8 s_prevWindowStatus = 0;
        const u8 ws = dMeter2Info_getWindowStatus();
        if (ws == 2 && s_prevWindowStatus != 2) {
            dMw_setRingPlayer(0);
        }
        s_prevWindowStatus = ws;
    }

    // Per-player HUD animation — match vanilla +1/-1 per-frame pattern
    // (or ±200 for oil, whose value range 0–9600 is much wider).
    for (PlayerId p = 0; p < MAX_LOCAL_PLAYERS; ++p) {
        if (!isValidPlayer(p) || !playerSlot(p)->joined) {
            continue;
        }
        const auto& res = inventory::resources(p);
        auto& anim = g_hudAnim[p];

        // Life (quarter-hearts)
        if (anim.displayLife < res.life) {
            anim.displayLife++;
        } else if (anim.displayLife > res.life) {
            anim.displayLife--;
        }

        // Max Life
        if (anim.displayMaxLife < res.maxLife) {
            anim.displayMaxLife++;
        } else if (anim.displayMaxLife > res.maxLife) {
            anim.displayMaxLife--;
        }

        // Magic
        if (anim.displayMagic < res.magic) {
            anim.displayMagic++;
        } else if (anim.displayMagic > res.magic) {
            anim.displayMagic--;
        }
        if (anim.displayMaxMagic < res.maxMagic) {
            anim.displayMaxMagic++;
        } else if (anim.displayMaxMagic > res.maxMagic) {
            anim.displayMaxMagic--;
        }

        // Rupees
        if (anim.displayRupees < res.rupees) {
            anim.displayRupees++;
        } else if (anim.displayRupees > res.rupees) {
            anim.displayRupees--;
        }

        // Oil — larger range, match vanilla +200/-200
        if (anim.displayOil < static_cast<s32>(res.oil)) {
            anim.displayOil += 200;
            if (anim.displayOil > static_cast<s32>(res.oil)) {
                anim.displayOil = static_cast<s32>(res.oil);
            }
        } else if (anim.displayOil > static_cast<s32>(res.oil)) {
            anim.displayOil -= 200;
            if (anim.displayOil < static_cast<s32>(res.oil)) {
                anim.displayOil = static_cast<s32>(res.oil);
            }
        }

        // Max Oil
        if (anim.displayMaxOil < static_cast<s32>(res.maxOil)) {
            anim.displayMaxOil += 200;
            if (anim.displayMaxOil > static_cast<s32>(res.maxOil)) {
                anim.displayMaxOil = static_cast<s32>(res.maxOil);
            }
        } else if (anim.displayMaxOil > static_cast<s32>(res.maxOil)) {
            anim.displayMaxOil -= 200;
            if (anim.displayMaxOil < static_cast<s32>(res.maxOil)) {
                anim.displayMaxOil = static_cast<s32>(res.maxOil);
            }
        }
    }
#endif
}

void drawView(ViewId view) {
#if !TARGET_PC
    (void)view;
    return;
#else
    if (g_hudDraw == nullptr) {
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

    f32 bottomAnchor = 0.0f;
#if TARGET_PC
    if (dusk::coop::render::isMultiViewActive()) {
        // Scale ortho height to match viewport aspect, preventing HUD squish.
        // Combined X scale after J2DPane scaling & projection = vpW/608.
        // Setting orthoH = 608 * vpH / vpW makes combined Y scale match.
        const f32 orthoH = (608.0f * vpH) / vpW;
        viewportOrtho.setOrtho(mDoGph_gInf_c::getMinXF(),
                               mDoGph_gInf_c::getMinYF(),
                               mDoGph_gInf_c::getWidthF(), orthoH,
                               -1.0f, 1.0f);
        bottomAnchor = orthoH - fbH;
    } else
#endif
    {
        viewportOrtho.setOrtho(0.0f, 0.0f, fbW, fbH, -1.0f, 1.0f);
    }
    viewportOrtho.setPort();
    dComIfGp_setCurrentGrafPort(&viewportOrtho);

    // Per-player HUD state setup — override the shared pane state (set by
    // dMeter2_c::_execute for player 0) with this view's player data.
    // X/Y/B item textures are patched in dMeter2Draw_c::draw() via
    // g_patchPlayerForDraw, where private member access is available.
    //
    // Life, magic, rupees, oil — use the per-player animated display values
    // so the HUD animates smoothly toward actual inventory values.
    const auto& anim = g_hudAnim[player];
    // Temporarily patch bottom-anchored HIO Y positions BEFORE drawRupee,
    // drawKey, drawButtonCross and draw() read them (they call paneTrans
    // immediately at call time). mLifeGaugePosY is NOT patched — top elements
    // stay at the top. mMagicMeterPosY/mLanternMeterPosY are passed explicitly
    // with bottomAnchor, so drawMagic/drawKantera don't read HIO for Y.
    // Restored after draw() so other viewports / single-view unaffected.
    const f32 savedMainBtnY = g_drawHIO.mMainHUDButtonsPosY;
    const f32 savedRingBtnY = g_drawHIO.mRingHUDButtonsPosY;
    const f32 savedCrossOffY = g_drawHIO.mButtonCrossOFFPosY;
    const f32 savedCrossOnY = g_drawHIO.mButtonCrossONPosY;
    const f32 savedRupeeKeyY = g_drawHIO.mRupeeKeyPosY;
    const f32 savedKeyY = g_drawHIO.mKeyPosY;
    if (bottomAnchor > 0.0f) {
        g_drawHIO.mMainHUDButtonsPosY += bottomAnchor;
        g_drawHIO.mRingHUDButtonsPosY += bottomAnchor;
        g_drawHIO.mButtonCrossOFFPosY += bottomAnchor;
        g_drawHIO.mButtonCrossONPosY += bottomAnchor;
        g_drawHIO.mRupeeKeyPosY += bottomAnchor;
        g_drawHIO.mKeyPosY += bottomAnchor;
    }

    g_hudDraw->drawLife(anim.displayMaxLife, anim.displayLife,
                         g_drawHIO.mLifeGaugePosX,
                         g_drawHIO.mLifeGaugePosY);
    g_hudDraw->drawMagic(anim.displayMaxMagic, anim.displayMagic,
                          g_drawHIO.mMagicMeterPosX,
                          g_drawHIO.mMagicMeterPosY + bottomAnchor);
    g_hudDraw->drawRupee(anim.displayRupees);
    g_hudDraw->drawKantera(anim.displayMaxOil, anim.displayOil,
                            g_drawHIO.mLanternMeterPosX,
                            g_drawHIO.mLanternMeterPosY + bottomAnchor);
    g_hudDraw->drawKey(dComIfGs_getKeyNum());

    // Signal dMeter2Draw_c::draw() to patch button-text panes for this
    // player before rendering the normal HUD content.
    g_patchPlayerForDraw = static_cast<s16>(player);

    g_hudDraw->draw();

    // Restore global HIO Y positions.
    if (bottomAnchor > 0.0f) {
        g_drawHIO.mMainHUDButtonsPosY = savedMainBtnY;
        g_drawHIO.mRingHUDButtonsPosY = savedRingBtnY;
        g_drawHIO.mButtonCrossOFFPosY = savedCrossOffY;
        g_drawHIO.mButtonCrossONPosY = savedCrossOnY;
        g_drawHIO.mRupeeKeyPosY = savedRupeeKeyY;
        g_drawHIO.mKeyPosY = savedKeyY;
    }

    // Draw per-view minimap (if available).
    // (Suppressed from the global draw list in dMeter2_c::_draw() on PC.)
    dMeter2_c* meter = dMeter2Info_getMeterClass();
    if (meter != nullptr) {
        dMeterMap_c* map = meter->getMap();
        if (map != nullptr) {
            const f32 savedMapY = map->getDrawPosY();
            if (bottomAnchor > 0.0f) {
                map->addDrawPosY(bottomAnchor);
            }
            map->draw();
            // Restore so the next viewport or global draw list gets the
            // correct original position (mDrawPosY was set by _move()).
            map->setDrawPosY(savedMapY);
        }
    }

    // Per-view emphasis button — created/updated/drawn here so the
    // content always reflects the current player for this view.
    auto& empView = g_viewEmpButton[view];
    if (empView == nullptr) {
        // Lazy-create on first draw of this view.
        dusk_coop_createEmpButton(&empView);
        dusk_coop_finalizeEmpButton();
    }

    if (empView != nullptr) {
        const auto& btnState = g_playerButtonState[player];
        EmphasisButtonParams params;
        params.mStatus       = 0;
        params.doStatus      = btnState.doStatus;
        params.aStatus       = btnState.aStatus;
        params.rStatus       = btnState.rStatus;
        params.zStatus       = btnState.zStatus;
        params.m3dStatus     = btnState.m3dStatus;
        params.cStickStatus  = btnState.cStickStatus;
        params.sButtonStatus = btnState.sButtonStatus;
        params.xItemStatus   = btnState.xStatus;
        params.yItemStatus   = btnState.yStatus;
        params.bottleStatus  = btnState.bottleStatus;

        dusk_coop_processEmphasisButton(empView, g_hudDraw, params);
        // Bottom-anchor the emphasis button position.
        // updateButton() reads g_drawHIO.mEmpButton.mEmpButtonPosY but is
        // only called from _execute() — not run for per-view buttons on PC.
        if (bottomAnchor > 0.0f) {
            empView->paneTrans(empView->mpParent,
                               empView->mParentCenterX,
                               g_drawHIO.mEmpButton.mEmpButtonPosY + bottomAnchor,
                               0xFF);
        }
        dusk_coop_drawEmpButton(empView);
    }

    g_patchPlayerForDraw = -1;

    dComIfGp_setCurrentGrafPort(reinterpret_cast<J2DOrthoGraph*>(prevGraf));
#endif
}

}  // namespace dusk::coop::hud
