#pragma once

/**
 * \file module.h
 * Net module lifecycle for the game build (deepseek m1).
 *
 * ENet requires a one-time enet_initialize()/enet_deinitialize() pair: a no-op
 * on macOS/Linux but REQUIRED on Windows (WSAStartup + timeBeginPeriod in
 * third_party/enet/win32.c) before any host can be created. The selftest owns
 * its own main; the game wires these through dusk-owned lifecycle call sites
 * (dusk::registerSettings -> initialize, dusk::config::shutdown -> shutdown).
 */

namespace dusk::net {

/// Calls enet_initialize() once at game startup. Returns true on success.
/// Idempotent across repeated calls.
bool initialize();

/// Calls enet_deinitialize() at game teardown. Safe to call when the module
/// was never initialized.
void shutdown();

}  // namespace dusk::net
