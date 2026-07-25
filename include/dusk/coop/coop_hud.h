#pragma once

#include "dusk/coop/coop_types.h"
#include <cstring>

class dMeterButton_c;
class dMeter2Draw_c;

// Emphasis-button lifecycle helpers (defined in d_meter2.cpp).
// Out-of-line helpers are needed because coop_hud cannot include
// d_meter_button.h (deep include chain conflicts).
// Buttons are allocated from the main 2D exp heap and are persistent
// (created lazily, destroyed only on coop reset) — the vanilla
// subheap2D(8) heap-lock dance destroys the heap on release and must
// not be used for these.
void dusk_coop_createEmpButton(dMeterButton_c** outBtn);
void dusk_coop_destroyEmpButton(dMeterButton_c** outBtn);
void dusk_coop_drawEmpButton(dMeterButton_c* btn);

// Per-player status parameters for the full emphasis-button processing
// logic from dMeter2_c::_execute().  All fields map 1:1 to the member
// variables read by the original emphasis block.
struct EmphasisButtonParams {
    u8 doStatus;        // A button action
    u8 aStatus;         // B button action (naming matches mAStatus in meter)
    u8 rStatus;
    u8 zStatus;
    u8 m3dStatus;       // 3D button action
    u8 cStickStatus;
    u8 sButtonStatus;
    u8 xItemStatus;     // item status for X slot (mItemStatus[1])
    u8 yItemStatus;     // item status for Y slot (mItemStatus[3])
    u8 bottleStatus;
    u32 mStatus;        // game-state bitfield (pass 0 to disable extra gating)
};

// Run the full vanilla emphasis-button update logic on a single button
// instance, using the supplied per-player status values instead of the
// meter class members.  This replaces the simplified execEmpButton.
void dusk_coop_processEmphasisButton(dMeterButton_c* btn, dMeter2Draw_c* draw,
                                     const EmphasisButtonParams& params);

// Per-player emphasis button instances declared at global scope to avoid
// C++ injecting a nested type into the namespace.
extern dMeterButton_c* g_playerEmpButton[8];

// Per-view emphasis button instances — indexed by ViewId, created lazily
// in drawView() and updated with the current player's per-frame state.
// Using per-view instances avoids cross-player state issues.
extern dMeterButton_c* g_viewEmpButton[8];

namespace dusk::coop::hud {

// Per-player button/status state captured from each player's Link actor.
// The daAlink_c setter methods (setDoStatus, setBStatus, setRStatus) write
// to these arrays alongside the global dComIfG_item_info_class so that the
// per-view HUD draw can read back each player's intended button states.
struct PlayerHudButtonState {
    u8 doStatus = 0;       // A button action
    u8 aStatus = 0;        // B button action
    u8 rStatus = 0;        // R button action
    u8 zStatus = 0;        // Z button action
    u8 m3dStatus = 0;      // 3D button action
    u8 xStatus = 0;        // X button
    u8 yStatus = 0;        // Y button
    u8 bottleStatus = 0;   // Bottle
    u8 cStickStatus = 0;   // C-stick
    u8 sButtonStatus = 0;  // S button
    u8 doSetFlag = 0;
    u8 aSetFlag = 0;
    u8 rSetFlag = 0;
    u8 zSetFlag = 0;      // Z (Midna) emphasis flag
    u8 m3dSetFlag = 0;    // 3D emphasis flag
    u8 xSetFlag = 0;      // X (wolf sense) emphasis flag
    u8 ySetFlag = 0;      // Y (wolf dig) emphasis flag
    u8 bottleSetFlag = 0; // Bottle emphasis flag
    u8 equipSword = 0;
    u8 itemSlotX = 0xFF;   // X-item slot
    u8 itemSlotY = 0xFF;   // Y-item slot
    u8 itemSelect = 0xFF;  // selected item
};

// Arrays indexed by PlayerId, written by daAlink_c setters and read by drawView().
extern PlayerHudButtonState g_playerButtonState[MAX_LOCAL_PLAYERS];

// Per-player HUD animation state — tracks displayed values that
// animate smoothly toward the actual inventory values (+1/-1 per frame
// for life/rupees, ±200 per frame for oil, matching the vanilla
// dMeter2_c::moveLife / moveRupee / moveKantera patterns).
struct HudAnimState {
    s16 displayLife = 12 * 4;     // quarter-hearts (vanilla start)
    s16 displayMaxLife = 12 * 4;
    s16 displayMagic = 0;
    s16 displayMaxMagic = 0;
    s16 displayRupees = 0;
    s32 displayOil = 0;
    s32 displayMaxOil = 0;
};

// Per-player HUD animation arrays.  Animated in tick(), read in drawView().
extern HudAnimState g_hudAnim[MAX_LOCAL_PLAYERS];

// When >= 0, dMeter2Draw_c::draw() patches A/B/R button pane text for this
// player before rendering the normal HUD content.  Set by drawView().
extern s16 g_patchPlayerForDraw;

void init();
void reset();
void tick();

// Draw per-player HUD for the given view within its viewport.
// Must be called inside a ScopedContext for the correct player.
void drawView(ViewId view);

}  // namespace dusk::coop::hud
