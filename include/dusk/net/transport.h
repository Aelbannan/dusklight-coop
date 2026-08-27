#pragma once

/**
 * \file transport.h
 * ENet transport (docs/design/mod-coop/00-network.md §1, §3).
 *
 * One ENetHost, owned by a dedicated socket thread. Cross-thread handoff is
 * via fixed-size SPSC ring buffers (no allocation in the hot path):
 *
 *   socket thread                      game thread
 *   enet_host_service()                 drain inbox  -> Session
 *   recv -> inbox rings                 build packets -> outbox rings
 *   outbox rings -> enet_peer_send()
 *
 * Peers are addressed by a stable slot index (0..kMaxPeers-1) assigned on
 * ENet connect, not by pointer. Stop() sets a stop flag, joins the socket
 * thread, and destroys the host there (enet_host_destroy resets all peers,
 * so shutdown never blocks on disconnect acks).
 *
 * Channel-split rings (M0.5): reliable traffic (channel 0 — control, events)
 * and unreliable snapshots (channel 1 — PlayerState)
 * never share a ring. Overflow policies:
 *   - reliable: a full ring is an EXPLICIT failure — Send() returns false,
 *     a counter bumps, and the session observes it. A reliable event is never
 *     silently dropped (review M0: deepseek M2, glm MAJOR-1).
 *   - connect/disconnect: a dedicated lifecycle inbox so a full reliable-data
 *     ring cannot drop a peer coming or going (zombie roster).
 *   - snapshots: per-source inbox + per-destination outbox; a full ring
 *     replace-newests that peer's newest *matching* snapshot (same
 *     PlayerState/HorseState playerId) so a multiplexed fan-out cannot
 *     drop player B to keep a stale player A. Falls back to newest-slot
 *     replace when no match exists (glm MAJOR-2). The host's O(remotes²)
 *     fan-out no longer shares one 128-slot outbox, and one sender cannot
 *     monopolize inbound snapshots.
 *
 * Peer-slot generation guard (deepseek M4): each slot carries a connection
 * generation, bumped when a slot is assigned. Every ring entry is stamped
 * with the slot generation at enqueue and dropped on mismatch at drain/poll,
 * so a packet enqueued for an old connection can never be delivered to the
 * new connection that reuses the slot. Generation is NOT bumped on release:
 * in-flight packets from the disconnecting peer (JoinReject racing
 * disconnect_later) must still reach the game thread.
 */

#include "dusk/net/protocol.h"

#include <enet/enet.h>

#include <atomic>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace dusk::net {

/// Sentinel for "no peer slot".
constexpr u8 kInvalidPeer = 0xFF;

/// Ring capacity for channel-0 (reliable) traffic. Control/events/combat are
/// sparse but correctness-critical; overflow is an explicit failure.
constexpr size_t kReliableRingCapacity = 64;
/// Connect/disconnect events get their own inbox so a full reliable-data
/// ring can never drop a peer coming or going (zombie roster / ignored rejoin).
constexpr size_t kLifecycleRingCapacity = 16;
/// Per-source inbound snapshot ring. One peer cannot monopolize a shared
/// inbox; 128 slots is the same hitch headroom as the matching outbox.
constexpr size_t kSnapshotInboxPerPeer = 128;
/// Per-destination outbound snapshot ring. Host fan-out is O(remotes²)
/// PlayerStates/frame into this slot; 128 per peer is ~18 frames at 7
/// packets/peer/frame (one pose per player) before replace-newest kicks in.
constexpr size_t kSnapshotOutboxPerPeer = 128;

/// Bound on the number of ENet events the socket thread processes before
/// draining the outbox, so a continuous inbound stream cannot starve
/// outbound drain (deepseek M6).
constexpr int kMaxEventsPerDrain = 32;

// ---------------------------------------------------------------------------
// SPSC ring buffer (single producer / single consumer, lock-free)
// ---------------------------------------------------------------------------

/// Fixed-capacity lock-free ring. Capacity must be a power of two. One
/// producer thread and one consumer thread only.
template <typename T, size_t Capacity>
class SPSCRing {
public:
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
    static_assert(Capacity >= 4, "replace-newest requires capacity >= 4");

    SPSCRing() = default;
    SPSCRing(const SPSCRing&) = delete;
    SPSCRing& operator=(const SPSCRing&) = delete;

    /// Producer side (drop-newest policy). Returns false — leaving the ring
    /// untouched — when full. Used for reliable traffic: a rejected push is
    /// an explicit failure the caller must surface, never a silent drop.
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

    /// Producer side (replace-newest policy) for snapshot traffic: appends
    /// `item`, or when full overwrites the newest slot in place so the ring
    /// always holds the freshest items. Returns true when an existing slot
    /// was replaced, false when appended. Never fails.
    ///
    /// Safe for capacity >= 4: when the ring is full it holds Capacity-1
    /// items, so the newest slot (head-1) is never the consumer's read
    /// position (tail) — the producer only ever overwrites a slot the
    /// consumer has not read.
    bool PushOrReplace(const T& item) {
        return PushOrReplaceMatching(item, [](const T&, const T&) { return false; });
    }

    /// Like PushOrReplace, but when full prefers overwriting the newest
    /// already-queued item for which `same(queued, item)` is true. Snapshot
    /// rings multiplex many players onto one peer; replacing an unrelated
    /// newest packet would drop that player's fresh pose and keep a stale
    /// one. The consumer's current read slot (`tail`) is never overwritten.
    /// Falls back to replacing the newest slot when no match is found.
    template <typename Same>
    bool PushOrReplaceMatching(const T& item, Same same) {
        const size_t mask = Capacity - 1;
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & mask;
        const size_t tail = tail_.load(std::memory_order_acquire);
        if (next != tail) {
            slots_[head] = item;
            head_.store(next, std::memory_order_release);
            return false;
        }
        size_t i = (head - 1) & mask;
        while (i != tail) {
            if (same(slots_[i], item)) {
                slots_[i] = item;
                return true;
            }
            i = (i - 1) & mask;
        }
        slots_[(head - 1) & mask] = item;
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

    /// Drop every queued item. Single-threaded only (call after the socket
    /// thread has joined, or before it is spawned).
    void Reset() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
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
/// empty and only `peerIndex` is meaningful. `generation` is the peer-slot
/// connection generation at enqueue; Transport::Poll drops stale Data packets
/// (slot reused by a newer connection) on mismatch.
struct InboundPacket {
    NetEventType type = NetEventType::Data;
    u8 peerIndex = kInvalidPeer;
    u8 channel = 0;
    u16 size = 0;
    u16 generation = 0;
    u8 data[kMaxMessageSize] = {};
};

/// Outbox slot: game thread -> socket thread. `generation` is stamped from
/// the peer slot at enqueue; DrainOutbox drops entries whose generation no
/// longer matches (slot freed and reused).
struct OutboundPacket {
    u8 peerIndex = kInvalidPeer;
    u8 channel = 0;
    u16 size = 0;
    u16 generation = 0;
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
    /// `channel`. Routes to the reliable (channel 0) or snapshot (channel 1)
    /// outbox ring; the socket thread drains it.
    ///   - reliable ring full: returns false and bumps ReliableOutboundDropped()
    ///     (explicit failure — never a silent drop);
    ///   - snapshot outbox (per peer) full: replaces the newest matching
    ///     PlayerState/HorseState for that playerId, else the newest entry
    ///     (returns true).
    /// Also returns false when the transport is not running or the message is
    /// too large.
    bool Send(u8 peerIndex, u8 channel, const void* data, u16 size);

    /// Game-thread: ask the socket thread to disconnect `peerIndex` after
    /// draining already-queued reliable packets (JoinReject then hang up).
    void DisconnectPeer(u8 peerIndex);

    /// Capstone MINOR F: why the most recent StartHost/StartClient failed
    /// ("" when none or when the last start succeeded). const char* to a
    /// static string, valid for the process lifetime. Lets the session/coop
    /// glue log start-failure causes distinctly (port-busy vs
    /// already-running vs resolve-failed).
    [[nodiscard]] const char* LastStartError() const { return lastStartError_; }

    /// Game-thread API: pop the next inbound packet (lifecycle first, then
    /// sticky disconnects from a full lifecycle inbox, then reliable data,
    /// then per-peer snapshots in round-robin). Drops stale Data packets
    /// whose peer slot was reused (bumps InboundGenerationDropped()).
    /// Returns false when empty.
    bool Poll(InboundPacket& out);

    // -- net stats (GLM MINOR-5 / deepseek M2: reliable drops are an error
    //    the session can observe) --
    /// Reliable outbound messages rejected because the reliable ring was full
    /// (explicit failure, never silent).
    [[nodiscard]] u64 ReliableOutboundDropped() const { return reliableOutboundDropped_.load(std::memory_order_relaxed); }
    /// Snapshot outbound messages that replaced a newer entry under pressure.
    [[nodiscard]] u64 SnapshotOutboundReplaced() const { return snapshotOutboundReplaced_.load(std::memory_order_relaxed); }
    /// Reliable inbound messages dropped because the reliable inbox was full.
    [[nodiscard]] u64 ReliableInboundDropped() const { return reliableInboundDropped_.load(std::memory_order_relaxed); }
    /// Connect/disconnect events that could not be queued (peer was reset on
    /// connect; disconnect is retried via a sticky bit so the session cannot
    /// miss a leave).
    [[nodiscard]] u64 LifecycleDropped() const { return lifecycleDropped_.load(std::memory_order_relaxed); }
    /// Snapshot inbound messages that replaced a newer entry under pressure.
    [[nodiscard]] u64 SnapshotInboundReplaced() const { return snapshotInboundReplaced_.load(std::memory_order_relaxed); }
    /// Inbound ENet packets larger than kMaxMessageSize, dropped at the socket.
    [[nodiscard]] u64 InboundOversized() const { return inboundOversized_.load(std::memory_order_relaxed); }
    /// Inbound packets dropped because the sending peer's slot was reused by
    /// a newer connection before the game thread drained them.
    [[nodiscard]] u64 InboundGenerationDropped() const { return inboundGenerationDropped_.load(std::memory_order_relaxed); }
    /// Outbound packets dropped at drain because the peer slot was reused.
    [[nodiscard]] u64 OutboundGenerationDropped() const { return outboundGenerationDropped_.load(std::memory_order_relaxed); }
    [[nodiscard]] u64 PacketsSent() const { return packetsSent_.load(std::memory_order_relaxed); }
    [[nodiscard]] u64 PacketsReceived() const { return packetsReceived_.load(std::memory_order_relaxed); }

private:
    void SocketThreadMain();
    void HandleSocketEvent(ENetEvent& event);
    void DrainOutbox();
    void SendPacket(const OutboundPacket& p);
    void ResetThreadSharedState();

    u8 AssignPeerSlot(ENetPeer* peer);
    void ReleasePeerSlot(u8 index);
    [[nodiscard]] u8 PeerSlot(ENetPeer* peer) const;
    [[nodiscard]] ENetPeer* PeerAt(u8 index) const;
    [[nodiscard]] u16 PeerGeneration(u8 index) const;

    ENetHost* host_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::mutex lifecycleMutex_;
    /// Capstone MINOR F: static reason string for the last failed start.
    const char* lastStartError_ = "";

    // Written only by the socket thread (the game thread never touches this).
    std::array<ENetPeer*, kMaxPeers> peerSlots_{};
    // Peer-slot connection generation: bumped on assign (socket thread);
    // read by the game thread to stamp Send() entries and filter Poll().
    // Not bumped on release — in-flight packets from the disconnecting peer
    // must still be delivered (JoinReject vs disconnect_later).
    std::array<std::atomic<u16>, kMaxPeers> peerGenerations_{};

    // The snapshot rings are ~7 MB (128 slots × 4 KB × 7 peers × 2 directions).
    // Heap-allocate them so a stack Session (selftest) cannot blow the 8 MB
    // default thread stack. The game keeps Session in static storage.
    struct Queues {
        SPSCRing<InboundPacket, kLifecycleRingCapacity> lifecycleInbox;
        SPSCRing<InboundPacket, kReliableRingCapacity> reliableInbox;
        std::array<SPSCRing<InboundPacket, kSnapshotInboxPerPeer>, kMaxPeers> snapshotInbox;
        SPSCRing<OutboundPacket, kReliableRingCapacity> reliableOutbox;
        std::array<SPSCRing<OutboundPacket, kSnapshotOutboxPerPeer>, kMaxPeers> snapshotOutbox;
        std::array<std::atomic<bool>, kMaxPeers> disconnectRequested{};
        std::atomic<u32> pendingDisconnectBits{0};
        u8 snapshotPollCursor = 0;
    };
    std::unique_ptr<Queues> q_{std::make_unique<Queues>()};

    std::atomic<u64> reliableOutboundDropped_{0};
    std::atomic<u64> snapshotOutboundReplaced_{0};
    std::atomic<u64> reliableInboundDropped_{0};
    std::atomic<u64> lifecycleDropped_{0};
    std::atomic<u64> snapshotInboundReplaced_{0};
    std::atomic<u64> inboundOversized_{0};
    std::atomic<u64> inboundGenerationDropped_{0};
    std::atomic<u64> outboundGenerationDropped_{0};
    std::atomic<u64> packetsSent_{0};
    std::atomic<u64> packetsReceived_{0};
};

}  // namespace dusk::net
