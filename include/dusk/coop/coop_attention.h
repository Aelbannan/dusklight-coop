#pragma once

#include "dusk/coop/coop_types.h"

class dAttention_c;

namespace dusk::coop::attention {

// One-time setup: allocate secondary attention systems.
void init();

// Tear down secondary attention systems.
void destroy();

// Call Run() for every joined player's attention system each frame.
void tick();

// Returns the attention system for the current coop context,
// or nullptr if coop is off / no context is active.
dAttention_c* forContext();

// Returns the attention system for a specific player.
// Player 0 always returns the original embedded instance.
dAttention_c* forPlayer(PlayerId player);

}  // namespace dusk::coop::attention
