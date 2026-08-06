#include "dusk/net/discovery.h"

#include <aurora/lib/logging.hpp>

#include <chrono>
#include <cstring>

// Socket platform shim: POSIX (macOS/Linux) vs Winsock (Windows).
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int SockLenT;
#define CLOSE_SOCKET closesocket
#define INVALID_SOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef socklen_t SockLenT;
#define CLOSE_SOCKET ::close
#define INVALID_SOCK (-1)
#endif

namespace dusk::net::discovery {

namespace {

aurora::Module DiscoveryLog("dusk::net::discovery");

constexpr char kMagic[4] = {'D', 'C', 'N', '1'};
constexpr int kAnnounceIntervalMs = 2000;
constexpr int kRecvTimeoutMs = 250;
constexpr size_t kMaxStoredSessions = 8;

std::atomic<u64> g_announcesReceived{0};
std::atomic<Listener*> g_activeListener{nullptr};

u64 NowMs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

bool InitSockets() {
#if defined(_WIN32)
    static bool init = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return init;
#else
    return true;
#endif
}

int MakeUdpSocket() {
    if (!InitSockets()) {
        return INVALID_SOCK;
    }
    const int s = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
    if (s == INVALID_SOCK) {
        DiscoveryLog.error("discovery: socket() failed");
        return INVALID_SOCK;
    }
    return s;
}

void SetReuseAddr(int s) {
    int one = 1;
#if defined(_WIN32)
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#else
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
}

void SetBroadcast(int s) {
    int one = 1;
#if defined(_WIN32)
    ::setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof(one));
#else
    ::setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
#endif
}

void SetRecvTimeout(int s, int ms) {
#if defined(_WIN32)
    DWORD tv = static_cast<DWORD>(ms);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv = {};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

bool FillSockAddr(sockaddr_in& out, const char* ip, u16 port) {
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    return ::inet_pton(AF_INET, ip, &out.sin_addr) == 1;
}

void BuildAnnounce(HostAnnounce& out, const std::string& name, u16 port, u8 players,
                   u8 maxPlayers) {
    std::memset(&out, 0, sizeof(out));
    std::memcpy(out.magic, kMagic, sizeof(kMagic));
    out.protocolVersion = kProtocolVersion;
    out.port = port;
    out.players = players;
    out.maxPlayers = maxPlayers;
    std::strncpy(out.name, name.c_str(), kMaxNameLength - 1);
    out.name[kMaxNameLength - 1] = '\0';
}

/// Serialize the announce as little-endian bytes (same byte order as the
/// protocol messages — hand-written, no padding surprises).
bool SerializeAnnounce(const HostAnnounce& a, u8 out[sizeof(HostAnnounce)]) {
    u8* p = out;
    std::memcpy(p, a.magic, 4);
    p += 4;
    *p++ = static_cast<u8>(a.protocolVersion & 0xFF);
    *p++ = static_cast<u8>((a.protocolVersion >> 8) & 0xFF);
    *p++ = static_cast<u8>(a.port & 0xFF);
    *p++ = static_cast<u8>((a.port >> 8) & 0xFF);
    *p++ = a.players;
    *p++ = a.maxPlayers;
    *p++ = a.reserved[0];
    *p++ = a.reserved[1];
    std::memcpy(p, a.name, kMaxNameLength);
    return true;
}

bool DeserializeAnnounce(const u8* data, size_t len, HostAnnounce& out) {
    if (len != sizeof(HostAnnounce)) {
        return false;
    }
    const u8* p = data;
    std::memcpy(out.magic, p, 4);
    p += 4;
    out.protocolVersion = static_cast<u16>(p[0] | (p[1] << 8));
    p += 2;
    out.port = static_cast<u16>(p[0] | (p[1] << 8));
    p += 2;
    out.players = *p++;
    out.maxPlayers = *p++;
    out.reserved[0] = *p++;
    out.reserved[1] = *p++;
    std::memcpy(out.name, p, kMaxNameLength);
    out.name[kMaxNameLength - 1] = '\0';
    return std::memcmp(out.magic, kMagic, sizeof(kMagic)) == 0 &&
           out.protocolVersion == kProtocolVersion;
}

}  // namespace

// ---------------------------------------------------------------------------
// Announcer
// ---------------------------------------------------------------------------

Announcer::~Announcer() {
    Stop();
}

void Announcer::Start(const std::string& name, u16 sessionPort, u8 maxPlayers) {
    Stop();
    name_ = name;
    sessionPort_ = sessionPort;
    maxPlayers_ = maxPlayers;
    stop_.store(false, std::memory_order_relaxed);
    thread_ = std::thread(&Announcer::ThreadMain, this);
    // Capstone MINOR J (review-full-deepseek-v4-flash-0731.md MINOR J): read
    // the LIVE player count — the glue seeds SetPlayers BEFORE Start (M4.5),
    // so a hardcoded "1" was stale the moment the announcer started.
    DiscoveryLog.info("discovery: announcing '{}' on port {} (players {}/{})", name, sessionPort,
        static_cast<u32>(players_.load(std::memory_order_relaxed)),
        static_cast<u32>(maxPlayers));
}

void Announcer::Stop() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Announcer::SetPlayers(u8 players) {
    players_.store(players, std::memory_order_relaxed);
}

void Announcer::ThreadMain() {
    const int s = MakeUdpSocket();
    if (s == INVALID_SOCK) {
        return;
    }
    SetBroadcast(s);
    sockaddr_in lanAddr;
    sockaddr_in loopAddr;
    const bool lanOk = FillSockAddr(lanAddr, "255.255.255.255", kAnnouncePort);
    const bool loopOk = FillSockAddr(loopAddr, "127.0.0.1", kAnnouncePort);

    HostAnnounce announce;
    BuildAnnounce(announce, name_, sessionPort_, 1, maxPlayers_);
    u8 buf[sizeof(HostAnnounce)];
    SerializeAnnounce(announce, buf);

    while (!stop_.load(std::memory_order_relaxed)) {
        announce.players = players_.load(std::memory_order_relaxed);
        SerializeAnnounce(announce, buf);
        if (lanOk) {
            ::sendto(s, reinterpret_cast<const char*>(buf), sizeof(buf), 0,
                reinterpret_cast<sockaddr*>(&lanAddr), sizeof(lanAddr));
        }
        if (loopOk) {
            ::sendto(s, reinterpret_cast<const char*>(buf), sizeof(buf), 0,
                reinterpret_cast<sockaddr*>(&loopAddr), sizeof(loopAddr));
        }
        // Sleep in small slices so Stop() joins promptly (<= 250 ms).
        for (int i = 0; i < kAnnounceIntervalMs / kRecvTimeoutMs; ++i) {
            if (stop_.load(std::memory_order_relaxed)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kRecvTimeoutMs));
        }
    }
    CLOSE_SOCKET(s);
    DiscoveryLog.info("discovery: announcer stopped");
}

// ---------------------------------------------------------------------------
// Listener
// ---------------------------------------------------------------------------

Listener::~Listener() {
    Stop();
}

void Listener::Start() {
    Stop();
    stop_.store(false, std::memory_order_relaxed);
    thread_ = std::thread(&Listener::ThreadMain, this);
    DiscoveryLog.info("discovery: listening for HostAnnounce on port {}", kAnnouncePort);
}

void Listener::Stop() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::vector<DiscoveredSession> Listener::Sessions() {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_;
}

void Listener::ThreadMain() {
    const int s = MakeUdpSocket();
    if (s == INVALID_SOCK) {
        return;
    }
    SetReuseAddr(s);
    SetRecvTimeout(s, kRecvTimeoutMs);
    sockaddr_in bindAddr;
    std::memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(kAnnouncePort);
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
        DiscoveryLog.warn("discovery: bind 0.0.0.0:{} failed (another listener?)", kAnnouncePort);
        CLOSE_SOCKET(s);
        return;
    }

    while (!stop_.load(std::memory_order_relaxed)) {
        u8 buf[sizeof(HostAnnounce)];
        sockaddr_in from;
        SockLenT fromLen = sizeof(from);
        const ssize_t n = ::recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
            reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n <= 0) {
            continue;  // timeout or error — re-check the stop flag
        }
        HostAnnounce announce;
        if (!DeserializeAnnounce(buf, static_cast<size_t>(n), announce)) {
            continue;  // not ours / version mismatch — ignore
        }
        char ipStr[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &from.sin_addr, ipStr, sizeof(ipStr));
        DiscoveredSession ds;
        ds.ip = ipStr;
        ds.port = announce.port;
        ds.players = announce.players;
        ds.maxPlayers = announce.maxPlayers;
        std::strncpy(ds.name, announce.name, kMaxNameLength - 1);
        ds.name[kMaxNameLength - 1] = '\0';
        ds.lastSeenMs = NowMs();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Replace a same-ip+port entry (refresh), keep the list bounded.
            bool refreshed = false;
            for (auto& existing : sessions_) {
                if (existing.ip == ds.ip && existing.port == ds.port) {
                    existing = ds;
                    refreshed = true;
                    break;
                }
            }
            if (!refreshed) {
                if (sessions_.size() >= kMaxStoredSessions) {
                    sessions_.erase(sessions_.begin());
                }
                sessions_.push_back(ds);
                DiscoveryLog.info("discovery: found session '{}' at {}:{} (players {}/{})",
                    ds.name, ds.ip, ds.port, static_cast<u32>(ds.players),
                    static_cast<u32>(ds.maxPlayers));
            }
        }
        g_announcesReceived.fetch_add(1, std::memory_order_relaxed);
    }
    CLOSE_SOCKET(s);
    DiscoveryLog.info("discovery: listener stopped");
}

u64 AnnouncesReceived() {
    return g_announcesReceived.load(std::memory_order_relaxed);
}

Listener* ActiveListener() {
    return g_activeListener.load(std::memory_order_relaxed);
}

void SetActiveListener(Listener* l) {
    g_activeListener.store(l, std::memory_order_relaxed);
}

}  // namespace dusk::net::discovery
