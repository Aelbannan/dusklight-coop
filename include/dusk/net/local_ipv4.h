#pragma once

/**
 * \file local_ipv4.h
 * Best-effort LAN IPv4 list for the Settings Network tab (host tells friends
 * what to type in Join Host IP). Empty when none can be detected.
 */

namespace dusk::net {

/// Comma-separated non-loopback IPv4 addresses. Valid until the next call.
const char* localIpv4Label();

}  // namespace dusk::net
