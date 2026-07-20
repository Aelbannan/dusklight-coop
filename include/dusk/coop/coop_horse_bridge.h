#pragma once

// Thin bridge so dComIfGp_getHorseActor inlines can resolve per-player horses
// without pulling the full co-op runtime into every TU.

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC

class daHorse_c;

namespace dusk::coop::horses {

// True when co-op is compiled in and enabled — route getHorseActor through context.
bool useCompatResolver();

daHorse_c* resolveForContextBridge();

// Raw global singleton (mPlayerPtr[HORSE_PTR]) — never context-routed.
daHorse_c* getGlobalHorseActor();

}  // namespace dusk::coop::horses

#endif  // ENABLE_LOCAL_COOP && TARGET_PC
