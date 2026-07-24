#pragma once

// Thin context bridge for the no-argument horse accessor.

#if TARGET_PC

class daHorse_c;

namespace dusk::coop::horses {

daHorse_c* resolveForContextBridge();

}  // namespace dusk::coop::horses

#endif  // TARGET_PC
