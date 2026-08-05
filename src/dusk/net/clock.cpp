#include "dusk/net/clock.h"

namespace dusk::net {
// NetClock is header-only; this TU exists so the CMake wiring has a stable
// clock source file and the selftest can link the module uniformly.
}  // namespace dusk::net
