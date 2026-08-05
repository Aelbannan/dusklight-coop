#include "dusk/net/module.h"

#include <aurora/lib/logging.hpp>

#include <enet/enet.h>

namespace dusk::net {

namespace {
aurora::Module NetLog("dusk::net::module");

bool g_initialized = false;
}  // namespace

bool initialize() {
    if (g_initialized) {
        return true;
    }
    if (enet_initialize() != 0) {
        NetLog.error("enet_initialize failed");
        return false;
    }
    g_initialized = true;
    NetLog.info("enet initialized (ENet {}.{}.{})", ENET_VERSION_MAJOR, ENET_VERSION_MINOR,
        ENET_VERSION_PATCH);
    return true;
}

void shutdown() {
    if (!g_initialized) {
        return;
    }
    enet_deinitialize();
    g_initialized = false;
    NetLog.info("enet deinitialized");
}

}  // namespace dusk::net
