#pragma once

/**
 * \file transport.h
 * ENet transport (docs/design/mod-coop/00-network.md §1, §3).
 *
 * One ENetHost, owned by a dedicated socket thread. Cross-thread handoff is
 * via fixed-size SPSC ring buffers (no allocation in the hot path):
 *
 *   socket thread                      game thread
 *   enet_host_service()                 drain inbox_  -> Session
 *   recv -> inbox_ ring                 build packets -> outbox_ ring
 *   outbox_ ring -> enet_peer_send()
 *
 * Peers are addressed by a stable slot index (0..kMaxPeers-1) assigned on
 * ENet connect, not by pointer. Stop() sets a stop flag, joins the socket
 * thread, and destroys the host there (enet_host_destroy resets all peers,
 * so shutdown never blocks on disconnect acks).
 */

#include "dusk/net/protocol.h"

#include <enet/enet.h>

#include <atomic>
#include <array>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace dusk::net {

/// Sentinel for "no peer slot".
constexpr u8 kInvalidPeer = 0xFF;

// ---------------------------------------------------------------------------
// SPSC ring buffer (single producer / single consumer, lock-free)
// ---------------------------------------------------------------------------

/// Fixed-capacity lock-free ring. Capacity must be a power of two. One
/// producer thread and one consumer thread only.
template <typename T, size_t Capacity>
class SPSCRing {
public:
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

    SPSCRing() = default;
    SPSCRing(const SPSCRing&) = delete;
    SPSCRing& operator=(const SPSCRing&) = delete;

    /// Producer side. Returns false (and leaves the ring untouched) when full.
    bool Push(const T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & (Capacity - 1);
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        slots_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    /// Consumer side. Returns false when empty.
    bool Pop(T& out) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        out = slots_[tail];
        tail_.store((tail + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

    [[nodiscard]] size_t Count() const {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (head + Capacity - tail) & (Capacity - 1);
    }

private:
    alignas(64) std::array<T, Capacity> slots_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

// ---------------------------------------------------------------------------
// Packets crossing the thread boundary
// ---------------------------------------------------------------------------

enum class NetEventType : u8 {
    Connected,
    Disconnected,
    Data,
};

/// Inbox slot: socket thread -> game thread. For Data, the full message
/// (header + payload) lives in `data`; for connect/disconnect events it is
/// empty and only `peerIndex` is meaningful.
struct InboundPacket {
    NetEventType type = NetEventType::Data;
    u8 peerIndex = kInvalidPeer;
    u8 channel = 0;
    u16 size = 0;
    u8 data[kMaxMessageSize] = {};
};

/// Outbox slot: game thread -> socket thread.
struct OutboundPacket {
    u8 peerIndex = kInvalidPeer;
    u8 channel = 0;
    u16 size = 0;
    u8 data[kMaxMessageSize] = {};
};

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

class Transport {
public:
    /// Max simultaneous ENet peers on the host (7 clients + the host itself).
    static constexpr size_t kMaxPeers = kMaxLocalPlayers - 1;

    Transport() = default;
    ~Transport();
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    /// Binds a listen socket on `port` (0 = ephemeral; see BoundPort()).
    /// `maxPeers` peers can connect at once.
    bool StartHost(u16 port, size_t maxPeers = kMaxPeers);
    /// Connects to `host`:`port` asynchronously; the ENET_EVENT_TYPE_CONNECT
    /// arrives on the socket thread and surfaces as NetEventType::Connected.
    bool StartClient(const std::string& host, u16 port);

    /// Graceful shutdown: stop flag -> join socket thread -> host destroyed
    /// on the socket thread. Idempotent, safe to call twice.
    void Stop();

    [[nodiscard]] bool IsRunning() const;
    /// Actual bound port (host mode; 0 before start or in client mode).
    [[nodiscard]] u16 BoundPort() const;

    /// Game-thread API: enqueue `data` to be sent to `peerIndex` on
    /// `channel`. Copies into the outbox ring; the socket thread drains it.
    /// Returns false when the ring is full or the transport is not running.
    bool Send(u8 peerIndex, u8 channel, const void* data, u16 size);

    /// Game-thread API: pop the next inbound packet. Returns false when empty.
    bool Poll(InboundPacket& out);

    [[nodiscard]] u64 InboundDropped() const { return inboundDropped_.load(std::memory_order_relaxed); }
    [[nodiscard]] u64 OutboundDropped() const { return outboundDropped_.load(std::memory_order_relaxed); }
    [[nodiscard]] u64 PacketsSent() const { return packetsSent_.load(std::memory_order_relaxed); }
    [[nodiscard]] u64 PacketsReceived() const { return packetsReceived_.load(std::memory_order_relaxed); }

private:
    void SocketThreadMain();
    void DrainOutbox();

    u8 AssignPeerSlot(ENetPeer* peer);
    void ReleasePeerSlot(u8 index);
    [[nodiscard]] u8 PeerSlot(ENetPeer* peer) const;
    [[nodiscard]] ENetPeer* PeerAt(u8 index) const;

    ENetHost* host_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::mutex lifecycleMutex_;

    // Written only by the socket thread (the game thread never touches this).
    std::array<ENetPeer*, kMaxPeers> peerSlots_{};

    SPSCRing<InboundPacket, 128> inbox_;
    SPSCRing<OutboundPacket, 128> outbox_;

    std::atomic<u64> inboundDropped_{0};
    std::atomic<u64> outboundDropped_{0};
    std::atomic<u64> packetsSent_{0};
    std::atomic<u64> packetsReceived_{0};
};

}  // namespace dusk::net
