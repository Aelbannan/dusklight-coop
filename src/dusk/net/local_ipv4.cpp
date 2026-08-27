#include "dusk/net/local_ipv4.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace dusk::net {
namespace {

bool IsLoopbackOrUnspecified(const char* ip) {
    return ip == nullptr || ip[0] == '\0' || std::strcmp(ip, "127.0.0.1") == 0 ||
           std::strcmp(ip, "0.0.0.0") == 0;
}

bool IsLinkLocal(const char* ip) {
    return ip != nullptr && std::strncmp(ip, "169.254.", 8) == 0;
}

void PushUnique(std::vector<std::string>& out, const char* ip) {
    if (IsLoopbackOrUnspecified(ip)) {
        return;
    }
    for (const auto& existing : out) {
        if (existing == ip) {
            return;
        }
    }
    out.emplace_back(ip);
}

void CollectIpv4(std::vector<std::string>& primary, std::vector<std::string>& linkLocal) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        return;
    }
    INTERFACE_INFO list[32];
    DWORD bytes = 0;
    const int ioctlOk =
        WSAIoctl(s, SIO_GET_INTERFACE_LIST, nullptr, 0, &list, sizeof(list), &bytes, nullptr, nullptr);
    closesocket(s);
    if (ioctlOk == SOCKET_ERROR) {
        return;
    }
    const int count = static_cast<int>(bytes / sizeof(INTERFACE_INFO));
    for (int i = 0; i < count; ++i) {
        if ((list[i].iiFlags & IFF_UP) == 0 || (list[i].iiFlags & IFF_LOOPBACK) != 0) {
            continue;
        }
        const sockaddr_in* addr = &list[i].iiAddress.AddressIn;
        char buf[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf)) == nullptr) {
            continue;
        }
        if (IsLinkLocal(buf)) {
            PushUnique(linkLocal, buf);
        } else {
            PushUnique(primary, buf);
        }
    }
#else
    ifaddrs* head = nullptr;
    if (getifaddrs(&head) != 0 || head == nullptr) {
        return;
    }
    for (ifaddrs* p = head; p != nullptr; p = p->ifa_next) {
        if (p->ifa_addr == nullptr || p->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((p->ifa_flags & IFF_UP) == 0 || (p->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        char buf[INET_ADDRSTRLEN] = {};
        const auto* addr = reinterpret_cast<const sockaddr_in*>(p->ifa_addr);
        if (inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf)) == nullptr) {
            continue;
        }
        if (IsLinkLocal(buf)) {
            PushUnique(linkLocal, buf);
        } else {
            PushUnique(primary, buf);
        }
    }
    freeifaddrs(head);
#endif
}

}  // namespace

const char* localIpv4Label() {
    static char buf[160];
    std::vector<std::string> primary;
    std::vector<std::string> linkLocal;
    CollectIpv4(primary, linkLocal);
    const std::vector<std::string>& ips = primary.empty() ? linkLocal : primary;
    if (ips.empty()) {
        buf[0] = '\0';
        return buf;
    }
    size_t n = 0;
    buf[0] = '\0';
    const size_t maxShow = ips.size() < 3 ? ips.size() : 3;
    for (size_t i = 0; i < maxShow && n < sizeof(buf); ++i) {
        n += static_cast<size_t>(std::snprintf(buf + n, sizeof(buf) - n, "%s%s", i == 0 ? "" : ", ",
            ips[i].c_str()));
    }
    return buf;
}

}  // namespace dusk::net
