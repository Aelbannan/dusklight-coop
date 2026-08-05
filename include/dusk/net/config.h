#pragma once

/**
 * \file config.h
 * Network co-op config CVars (docs/design/mod-coop/00-network.md §10) via the
 * project's config system (src/dusk/config_var.hpp / config.cpp).
 *
 * These are declared here so the net module itself stays free of the heavy
 * config implementation; definitions and registration live in
 * src/dusk/net/config.cpp (game build only — the selftest configures sessions
 * directly via SessionConfig).
 */

#include "dusk/config_var.hpp"

namespace dusk::net::config {

using ::dusk::config::ConfigVar;

/// Master switch; M4+ wires this to start/stop a session from the game.
extern ConfigVar<bool> enabled;
/// Host listen port (00-network.md: 44771 is reserved for the v1 broadcast
/// announce, so the session port defaults one below it).
extern ConfigVar<u16> hostPort;
/// Manual join target IP (00-network.md §12: manual host IP join is the v1
/// fallback until LAN discovery lands in M4).
extern ConfigVar<std::string> joinHost;
/// Host: session name. Client: player name sent in JoinRequest.
extern ConfigVar<std::string> sessionName;

/// Registers every net CVar with the dusk config registry. Called from
/// dusk::registerSettings() at startup.
void registerConfig();

}  // namespace dusk::net::config
