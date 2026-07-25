#pragma once

#include "dusk/coop/coop_types.h"
#include <cstring>

namespace dusk::coop::hud {

// Set per-frame to suppress the global HUD draw when per-view HUD is active.
extern bool g_perViewHudActive;

// Per-player button/status state captured from each player's Link actor.
// The daAlink_c setter methods (setDoStatus, setBStatus, setRStatus) write
// to these arrays alongside the global dComIfG_item_info_class so that the
// per-view HUD draw can read back each player's intended button states.
struct PlayerHudButtonState {
    u8 doStatus = 0;       // A button action
    u8 aStatus = 0;        // B button action
    u8 rStatus = 0;        // R button action
    u8 zStatus = 0;        // Z button action
    u8 xStatus = 0;        // X button
    u8 yStatus = 0;        // Y button
    u8 bottleStatus = 0;   // Bottle
    u8 cStickStatus = 0;   // C-stick
    u8 sButtonStatus = 0;  // S button
    u8 doSetFlag = 0;
    u8 aSetFlag = 0;
    u8 rSetFlag = 0;
    u8 equipSword = 0;
    u8 itemSlotX = 0xFF;   // X-item slot
    u8 itemSlotY = 0xFF;   // Y-item slot
    u8 itemSelect = 0xFF;  // selected item
};

// Arrays indexed by PlayerId, written by daAlink_c setters and read by drawView().
extern PlayerHudButtonState g_playerButtonState[MAX_LOCAL_PLAYERS];

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
