#pragma once

/**
 * \file discovery.h
 * M4 — LAN session discovery (00-network.md §4 HostAnnounce, port 44771).
 *
 * The world host broadcasts a fixed-size HostAnnounce datagram (~2 s
 * interval) to the LAN broadcast address AND to loopback (so host + client
 * on one machine find each other without a second device); clients listen on
 * port 44771, parse announces, log each new session, and store a bounded
 * list for the settings UI. LAN-simple by design: one UDP broadcast + one
 * listener, no NAT traversal, no mDNS. Manual IP join (net.joinHost) remains
 * the fallback.
 *
 * Threads: each of Announcer/Listener owns one socket thread; both are
 * stopped from the game thread (Stop() joins). The wire struct is
 * fixed-size and little-endian, hand-written like the protocol messages.
 */

#include "dusk/net/protocol.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dusk::net::discovery {

/// Reserved announce port (see include/dusk/net/config.h).
constexpr u16 kAnnouncePort = 44771;

/// Fixed-size announce datagram (44 B). Little-endian on the wire.
struct HostAnnounce {
    char magic[4];       // "DCN1" — Dusklight Co-op Net announce v1
    u16 protocolVersion; // kProtocolVersion (mismatched announces are ignored)
    u16 port;            // the session's ENet listen port
    u8 players;          // current player count
    u8 maxPlayers;       // roster cap
    u8 reserved[2];
    char name[kMaxNameLength];  // session name (NUL-padded)
};

/// One parsed HostAnnounce (see HostAnnounce).
struct DiscoveredSession {
    std::string ip;
    u16 port = 0;
    u8 players = 0;
    u8 maxPlayers = 0;
    char name[kMaxNameLength] = {};
    u64 lastSeenMs = 0;
};

/// Host-side periodic broadcaster. Start()/Stop() from the game thread;
/// SetPlayers() updates the next datagram (players joining/leaving).
class Announcer {
public:
    Announcer() = default;
    ~Announcer();
    Announcer(const Announcer&) = delete;
    Announcer& operator=(const Announcer&) = delete;

    void Start(const std::string& name, u16 sessionPort, u8 maxPlayers);
    void Stop();
    void SetPlayers(u8 players);
    [[nodiscard]] bool running() const { return !stop_.load(std::memory_order_relaxed); }

private:
    void ThreadMain();

    std::thread thread_;
    std::atomic<bool> stop_{true};
    std::atomic<u8> players_{0};
    u16 sessionPort_ = 0;
    u8 maxPlayers_ = kMaxLocalPlayers;
    std::string name_;
};

/// Client-side listener. Start()/Stop() from the game thread;
/// Sessions() returns the current (mutex-guarded) discovered list.
class Listener {
public:
    Listener() = default;
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    void Start();
    void Stop();
    std::vector<DiscoveredSession> Sessions();
    [[nodiscard]] bool running() const { return !stop_.load(std::memory_order_relaxed); }

private:
    void ThreadMain();

    std::thread thread_;
    std::atomic<bool> stop_{true};
    std::mutex mutex_;
    std::vector<DiscoveredSession> sessions_;
};

/// The listener the coop glue currently runs (registered by coop.cpp so the
/// settings UI can list discovered sessions), or nullptr when no client
/// session is listening.
Listener* ActiveListener();
void SetActiveListener(Listener* l);

/// How many received datagrams were accepted (tests + diagnostics).
u64 AnnouncesReceived();

}  // namespace dusk::net::discovery
