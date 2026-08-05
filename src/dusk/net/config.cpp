#include "dusk/net/config.h"

#include "dusk/config.hpp"

namespace dusk::net::config {

ConfigVar<bool> enabled{"net.enabled", false};
ConfigVar<u16> hostPort{"net.hostPort", 44770};
ConfigVar<std::string> joinHost{"net.joinHost", "127.0.0.1"};
ConfigVar<std::string> sessionName{"net.sessionName", "Dusklight co-op"};

void registerConfig() {
    Register(enabled);
    Register(hostPort);
    Register(joinHost);
    Register(sessionName);
}

}  // namespace dusk::net::config
