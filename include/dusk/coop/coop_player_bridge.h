#pragma once

class fopAc_ac_c;

namespace dusk::coop {

// Resolve the Link stored in the currently scoped player slot.
fopAc_ac_c* playerForContextBridge();
int playerContextIndexBridge();

}  // namespace dusk::coop
