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

/// Persisted: on launch, start a client session to joinHost:hostPort.
/// Flipping this in-game does not start or stop a live session. Hosting is
/// always an explicit Host press. Never auto-hosts.
extern ConfigVar<bool> autoConnect;
/// This-process session intent. Host/Connect set the in-memory connected
/// bit rather than this CVar, so it is not written to config.json. Launch
/// `--cvar net.connected=true` (Override layer) starts a session this run
/// using net.role.
extern ConfigVar<bool> connected;
/// Host listen port. On a client this is the host's port — the connect
/// target is `net.joinHost`:`net.hostPort` (the client transport never
/// binds; an ephemeral port is used, so host and client may share the same
/// value on one machine).
extern ConfigVar<u16> hostPort;
/// Manual join target IP or hostname. Empty is a hard fail (not localhost).
/// Port belongs in net.hostPort, not in this string.
extern ConfigVar<std::string> joinHost;
/// Host: session name in the roster. Client: player name sent in JoinRequest.
extern ConfigVar<std::string> sessionName;
/// Session role: "host" or "client". Set by Host / Connect. Autostart from
/// net.autoConnect always connects as client regardless of this value.
extern ConfigVar<std::string> role;
/// Legacy alias for the old master switch. Loaded so config.json /
/// `--cvar net.enabled=true` can migrate; not shown in the Network tab.
extern ConfigVar<bool> enabled;

/// Registers every net CVar with the dusk config registry. Called from
/// dusk::registerSettings() at startup.
void registerConfig();

}  // namespace dusk::net::config
