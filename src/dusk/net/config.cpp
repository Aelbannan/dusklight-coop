#include "dusk/net/config.h"

#include "dusk/config.hpp"

namespace dusk::net::config {

ConfigVar<bool> autoConnect{"net.autoConnect", false};
ConfigVar<bool> connected{"net.connected", false};
ConfigVar<bool> enabled{"net.enabled", false};
ConfigVar<u16> hostPort{"net.hostPort", 44770};
ConfigVar<std::string> joinHost{"net.joinHost", "127.0.0.1"};
ConfigVar<std::string> sessionName{"net.sessionName", "Dusklight co-op"};
ConfigVar<std::string> role{"net.role", "host"};

void registerConfig() {
    Register(autoConnect);
    Register(connected);
    Register(enabled);
    Register(hostPort);
    Register(joinHost);
    Register(sessionName);
    Register(role);
}

}  // namespace dusk::net::config
