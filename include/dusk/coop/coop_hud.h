#pragma once

#include "dusk/coop/coop_types.h"

namespace dusk::coop::hud {

// Set per-frame to suppress the global HUD draw when per-view HUD is active.
extern bool g_perViewHudActive;

void init();
void reset();
void tick();

// Draw per-player HUD for the given view within its viewport.
// Must be called inside a ScopedContext for the correct player.
void drawView(ViewId view);

}  // namespace dusk::coop::hud
