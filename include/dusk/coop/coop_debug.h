#pragma once

#include "dusk/coop/coop_types.h"

#include <cassert>

namespace dusk::coop::debug {

void init();
void reset();
void drawOverlay();

void logInfo(const char* fmt, ...);
void logWarn(const char* fmt, ...);
void logError(const char* fmt, ...);

}  // namespace dusk::coop::debug

#if TARGET_PC && defined(DEBUG)
#define COOP_ASSERT(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            dusk::coop::debug::logError("COOP_ASSERT failed: %s (%s:%d)", #cond, __FILE__,         \
                                        __LINE__);                                                 \
            assert(cond);                                                                          \
        }                                                                                          \
    } while (0)
#else
#define COOP_ASSERT(cond) ((void)0)
#endif
