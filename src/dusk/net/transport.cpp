#include "dusk/net/transport.h"

#include <aurora/lib/logging.hpp>

#include <chrono>
#include <cstring>

namespace dusk::net {

namespace {
aurora::Module NetLog("dusk::net::transport");

constexpr int kServiceTimeoutMs = 8;  // upper bound on socket-thread wakeup latency

/// Snapshot identity: PlayerState/HorseState packets for the same player
/// replace each other under ring pressure. Other channel-1 payloads (tests,
/// unknown) match on type so a flood still coalesces.
bool SameSnapshotIdentity(const u8* a, u16 aSize, const u8* b, u16 bSize) {
    if (aSize < 5 || bSize < 5) {
        return false;
    }
    const u16 typeA = static_cast<u16>(a[0] | (a[1] << 8));
    const u16 typeB = static_cast<u16>(b[0] | (b[1] << 8));
    if (typeA != typeB) {
        return false;
    }
    if (typeA == static_cast<u16>(MsgType::PlayerState) ||
        typeA == static_cast<u16>(MsgType::HorseState))
    {
        return a[4] == b[4];
    }
    return true;
}

u64 NowUs() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
}  // namespace

Transport::~Transport() {
    Stop();
}

bool Transport::StartHost(u16 port, size_t maxPeers) {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    lastStartError_ = "";
    if (host_ != nullptr) {
        NetLog.warn("start host called on an already-running transport");
        lastStartError_ = "transport already running";
        return false;
    }
    if (maxPeers == 0 || maxPeers > kMaxPeers) {
        maxPeers = kMaxPeers;
    }

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;

    ENetHost* host = enet_host_create(&address, maxPeers, /*channelLimit=*/2, 0, 0);
    if (host == nullptr) {
        NetLog.error("enet_host_create failed for port {}", port);
        lastStartError_ = "listen failed (port busy or no sockets)";
        return false;
    }
    host_ = host;
    stop_.store(false, std::memory_order_relaxed);
    peerSlots_.fill(nullptr);
    for (auto& gen : peerGenerations_) {
        gen.store(0, std::memory_order_relaxed);
    }
    ResetThreadSharedState();
    // Visible before the thread spawns so IsRunning() is true immediately
    // after StartHost returns (GLM MINOR-2).
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread(&Transport::SocketThreadMain, this);
    NetLog.info("host transport up on port {}", host_->address.port);
    return true;
}

bool Transport::StartClient(const std::string& host, u16 port) {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    lastStartError_ = "";
    if (host_ != nullptr) {
        NetLog.warn("start client called on an already-running transport");
        lastStartError_ = "transport already running";
        return false;
    }

    ENetHost* clientHost = enet_host_create(nullptr, /*peerCount=*/1, /*channelLimit=*/2, 0, 0);
    if (clientHost == nullptr) {
        NetLog.error("enet_host_create failed for client");
        lastStartError_ = "socket create failed";
        return false;
    }

    ENetAddress address;
    if (enet_address_set_host(&address, host.c_str()) != 0) {
        NetLog.error("enet_address_set_host failed for '{}'", host);
        lastStartError_ = "cannot resolve join host";
        enet_host_destroy(clientHost);
        return false;
    }
    address.port = port;

    ENetPeer* peer = enet_host_connect(clientHost, &address, /*channelCount=*/2, 0);
    if (peer == nullptr) {
        NetLog.error("enet_host_connect failed for {}:{}", host, port);
        lastStartError_ = "connect failed";
        enet_host_destroy(clientHost);
        return false;
    }

    host_ = clientHost;
    stop_.store(false, std::memory_order_relaxed);
    peerSlots_.fill(nullptr);
    for (auto& gen : peerGenerations_) {
        gen.store(0, std::memory_order_relaxed);
    }
    ResetThreadSharedState();
    // Visible before the thread spawns (GLM MINOR-2); the client's single
    // peer is not yet "connected" — the CONNECT event on the socket thread
    // assigns it slot 0.
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread(&Transport::SocketThreadMain, this);
    NetLog.info("client transport connecting to {}:{}", host, port);
    return true;
}

void Transport::Stop() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (host_ == nullptr) {
        return;
    }
    NetLog.info("stopping transport");
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
    host_ = nullptr;
    running_.store(false, std::memory_order_relaxed);
    ResetThreadSharedState();
}

bool Transport::IsRunning() const {
    return running_.load(std::memory_order_relaxed);
}

u16 Transport::BoundPort() const {
    if (host_ == nullptr) {
        return 0;
    }
    return host_->address.port;
}

bool Transport::Send(u8 peerIndex, u8 channel, const void* data, u16 size) {
    if (host_ == nullptr || size > kMaxMessageSize || peerIndex >= kMaxPeers) {
        return false;
    }
    OutboundPacket p;
    p.peerIndex = peerIndex;
    p.channel = channel;
    p.size = size;
    p.generation = PeerGeneration(peerIndex);
    std::memcpy(p.data, data, size);
    if (channel == kChannelUnreliable) {
        // Snapshots: per-peer outbox, replace matching playerId under pressure.
        auto same = [](const OutboundPacket& x, const OutboundPacket& y) {
            return SameSnapshotIdentity(x.data, x.size, y.data, y.size);
        };
        if (q_->snapshotOutbox[peerIndex].PushOrReplaceMatching(p, same)) {
            snapshotOutboundReplaced_.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }
    // Reliable: a full ring is an explicit, visible failure, never a silent
    // drop (deepseek M2 / glm MAJOR-1). The counter is the session-visible
    // error signal.
    if (!q_->reliableOutbox.Push(p)) {
        reliableOutboundDropped_.fetch_add(1, std::memory_order_relaxed);
        NetLog.warn("reliable outbox ring full; dropping {} bytes to peer {} (explicit failure)",
            size, peerIndex);
        return false;
    }
    return true;
}

void Transport::DisconnectPeer(u8 peerIndex) {
    if (peerIndex >= kMaxPeers) {
        return;
    }
    q_->disconnectRequested[peerIndex].store(true, std::memory_order_release);
}

void Transport::ResetThreadSharedState() {
    q_->pendingDisconnectBits.store(0, std::memory_order_relaxed);
    q_->snapshotPollCursor = 0;
    for (auto& flag : q_->disconnectRequested) {
        flag.store(false, std::memory_order_relaxed);
    }
    q_->lifecycleInbox.Reset();
    q_->reliableInbox.Reset();
    q_->reliableOutbox.Reset();
    for (auto& ring : q_->snapshotInbox) {
        ring.Reset();
    }
    for (auto& ring : q_->snapshotOutbox) {
        ring.Reset();
    }
}

bool Transport::Poll(InboundPacket& out) {
    // Lifecycle (connect/disconnect) first — must not sit behind a full
    // reliable-data ring. Then sticky disconnects from a full lifecycle
    // inbox (so a missed DISCONNECT cannot leave peerToPlayer_ mapped).
    // Then reliable data, then per-peer snapshots in round-robin.
    for (;;) {
        if (!q_->lifecycleInbox.Pop(out)) {
            break;
        }
        return true;
    }
    {
        const u32 bits = q_->pendingDisconnectBits.load(std::memory_order_acquire);
        if (bits != 0) {
            for (u8 i = 0; i < kMaxPeers; ++i) {
                const u32 mask = 1u << i;
                if ((bits & mask) == 0) {
                    continue;
                }
                q_->pendingDisconnectBits.fetch_and(~mask, std::memory_order_acq_rel);
                out = InboundPacket{};
                out.type = NetEventType::Disconnected;
                out.peerIndex = i;
                return true;
            }
        }
    }
    for (;;) {
        if (!q_->reliableInbox.Pop(out)) {
            break;
        }
        if (out.type == NetEventType::Data && out.generation != PeerGeneration(out.peerIndex)) {
            inboundGenerationDropped_.fetch_add(1, std::memory_order_relaxed);
            continue;  // slot reused by a newer connection; drop the stale packet
        }
        return true;
    }
    for (size_t n = 0; n < kMaxPeers; ++n) {
        const u8 i = static_cast<u8>((q_->snapshotPollCursor + n) % kMaxPeers);
        if (!q_->snapshotInbox[i].Pop(out)) {
            continue;
        }
        q_->snapshotPollCursor = static_cast<u8>((i + 1) % kMaxPeers);
        if (out.type == NetEventType::Data && out.generation != PeerGeneration(out.peerIndex)) {
            inboundGenerationDropped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Socket thread
// ---------------------------------------------------------------------------

void Transport::SocketThreadMain() {
    // running_ was set under the lifecycle mutex before the thread spawned
    // (GLM MINOR-2); nothing to do here.
    while (!stop_.load(std::memory_order_relaxed)) {
        // Process at most kMaxEventsPerDrain events, then drain the outbox:
        // a continuous inbound stream cannot starve outbound drain (deepseek
        // M6). The first service call of each burst waits up to the timeout;
        // the rest are non-blocking.
        int processed = 0;
        while (!stop_.load(std::memory_order_relaxed)) {
            ENetEvent event;
            const int result =
                enet_host_service(host_, &event, processed == 0 ? kServiceTimeoutMs : 0);
            if (result <= 0) {
                break;  // no (more) events in this burst
            }
            HandleSocketEvent(event);
            if (++processed >= kMaxEventsPerDrain) {
                break;  // bounded burst; drain below, then service again
            }
        }
        DrainOutbox();
    }
    // One final drain so goodbye messages (SessionEnd/PlayerLeave) enqueued by
    // the game thread right before Stop() still go out.
    DrainOutbox();
    // enet_host_destroy resets every peer, so this never blocks on disconnect
    // acks; run it here (on the socket thread) after the service loop exits.
    enet_host_destroy(host_);
    NetLog.info("transport socket thread exited");
}

void Transport::HandleSocketEvent(ENetEvent& event) {
    switch (event.type) {
    case ENET_EVENT_TYPE_CONNECT: {
        const u8 index = AssignPeerSlot(event.peer);
        if (index == kInvalidPeer) {
            NetLog.warn("peer limit reached; resetting incoming connection");
            enet_peer_reset(event.peer);
            break;
        }
        InboundPacket pkt;
        pkt.type = NetEventType::Connected;
        pkt.peerIndex = index;
        pkt.size = 0;
        pkt.generation = PeerGeneration(index);
        // A later CONNECT on this slot means the previous occupant's
        // disconnect (if it was dropped) must not fire after we hand this
        // connect to the session.
        q_->pendingDisconnectBits.fetch_and(~(1u << index), std::memory_order_relaxed);
        if (!q_->lifecycleInbox.Push(pkt)) {
            lifecycleDropped_.fetch_add(1, std::memory_order_relaxed);
            NetLog.warn("lifecycle inbox full; resetting peer {} (connect event dropped)", index);
            enet_peer_reset(event.peer);
            ReleasePeerSlot(index);
            break;
        }
        NetLog.info("peer {} connected", index);
        break;
    }
    case ENET_EVENT_TYPE_DISCONNECT: {
        const u8 index = PeerSlot(event.peer);
        ReleasePeerSlot(index);
        InboundPacket pkt;
        pkt.type = NetEventType::Disconnected;
        pkt.peerIndex = index;
        pkt.size = 0;
        if (!q_->lifecycleInbox.Push(pkt)) {
            lifecycleDropped_.fetch_add(1, std::memory_order_relaxed);
            q_->pendingDisconnectBits.fetch_or(1u << index, std::memory_order_relaxed);
            NetLog.warn("lifecycle inbox full; sticky-disconnect armed for peer {}", index);
        }
        NetLog.info("peer {} disconnected", index);
        break;
    }
    case ENET_EVENT_TYPE_RECEIVE: {
        const u8 index = PeerSlot(event.peer);
        if (index == kInvalidPeer) {
            enet_packet_destroy(event.packet);
            break;
        }
        if (event.packet->dataLength > kMaxMessageSize) {
            // Separate, visible metric for oversized inbound packets (GLM
            // MINOR-5): a misbehaving/legacy peer is not silently ignored.
            inboundOversized_.fetch_add(1, std::memory_order_relaxed);
            NetLog.warn("oversized inbound packet ({} bytes) from peer {} dropped",
                event.packet->dataLength, index);
            enet_packet_destroy(event.packet);
            break;
        }
        InboundPacket pkt;
        pkt.type = NetEventType::Data;
        pkt.peerIndex = index;
        pkt.channel = static_cast<u8>(event.channelID);
        pkt.size = static_cast<u16>(event.packet->dataLength);
        pkt.generation = PeerGeneration(index);
        std::memcpy(pkt.data, event.packet->data, pkt.size);
        if (pkt.channel == kChannelUnreliable) {
            auto same = [](const InboundPacket& x, const InboundPacket& y) {
                return SameSnapshotIdentity(x.data, x.size, y.data, y.size);
            };
            if (index < kMaxPeers && q_->snapshotInbox[index].PushOrReplaceMatching(pkt, same)) {
                snapshotInboundReplaced_.fetch_add(1, std::memory_order_relaxed);
            }
            packetsReceived_.fetch_add(1, std::memory_order_relaxed);
        } else if (q_->reliableInbox.Push(pkt)) {
            packetsReceived_.fetch_add(1, std::memory_order_relaxed);
        } else {
            reliableInboundDropped_.fetch_add(1, std::memory_order_relaxed);
            NetLog.warn("reliable inbox ring full; dropping {} bytes from peer {}", pkt.size, index);
        }
        enet_packet_destroy(event.packet);
        break;
    }
    case ENET_EVENT_TYPE_NONE:
        break;
    }
}

void Transport::DrainOutbox() {
    // Reliable (control/events) first, then per-peer snapshots.
    OutboundPacket p;
    while (q_->reliableOutbox.Pop(p)) {
        SendPacket(p);
    }
    for (size_t i = 0; i < kMaxPeers; ++i) {
        while (q_->snapshotOutbox[i].Pop(p)) {
            SendPacket(p);
        }
    }
    if (host_ != nullptr) {
        enet_host_flush(host_);
    }
    for (size_t i = 0; i < kMaxPeers; ++i) {
        if (!q_->disconnectRequested[i].exchange(false, std::memory_order_acq_rel)) {
            continue;
        }
        ENetPeer* peer = PeerAt(static_cast<u8>(i));
        if (peer != nullptr) {
            enet_peer_disconnect_later(peer, 0);
        }
    }
}

void Transport::SendPacket(const OutboundPacket& p) {
    ENetPeer* peer = PeerAt(p.peerIndex);
    if (peer == nullptr) {
        return;  // peer vanished between enqueue and drain; nothing to send to
    }
    if (p.generation != PeerGeneration(p.peerIndex)) {
        // The slot was freed and reused by a newer connection after this entry
        // was enqueued — never misdeliver a stale packet (deepseek M4).
        outboundGenerationDropped_.fetch_add(1, std::memory_order_relaxed);
        NetLog.warn("dropping stale outbound packet for peer {} (slot reused)", p.peerIndex);
        return;
    }
    const enet_uint32 flags = (p.channel == kChannelReliable) ? ENET_PACKET_FLAG_RELIABLE : 0;
    ENetPacket* packet = enet_packet_create(p.data, p.size, flags);
    if (packet == nullptr) {
        NetLog.warn("enet_packet_create failed for {} bytes", p.size);
        return;
    }
    if (enet_peer_send(peer, p.channel, packet) < 0) {
        enet_packet_destroy(packet);
        NetLog.warn("enet_peer_send failed for peer {}", p.peerIndex);
        return;
    }
    packetsSent_.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Peer slot management (socket thread only)
// ---------------------------------------------------------------------------

u8 Transport::AssignPeerSlot(ENetPeer* peer) {
    for (size_t i = 0; i < kMaxPeers; ++i) {
        if (peerSlots_[i] == nullptr) {
            peerSlots_[i] = peer;
            peerGenerations_[i].fetch_add(1, std::memory_order_relaxed);
            return static_cast<u8>(i);
        }
    }
    return kInvalidPeer;
}

void Transport::ReleasePeerSlot(u8 index) {
    if (index < kMaxPeers) {
        peerSlots_[index] = nullptr;
        // Do not bump generation here. AssignPeerSlot bumps on reuse, which
        // is enough to drop stale packets. Bumping on release made Poll()
        // drop in-flight JoinReject after disconnect_later (the socket
        // thread releases the slot before the game thread drains the inbox).
    }
}

u8 Transport::PeerSlot(ENetPeer* peer) const {
    for (size_t i = 0; i < kMaxPeers; ++i) {
        if (peerSlots_[i] == peer) {
            return static_cast<u8>(i);
        }
    }
    return kInvalidPeer;
}

ENetPeer* Transport::PeerAt(u8 index) const {
    if (index >= kMaxPeers) {
        return nullptr;
    }
    return peerSlots_[index];
}

u16 Transport::PeerGeneration(u8 index) const {
    if (index >= kMaxPeers) {
        return 0;
    }
    return peerGenerations_[index].load(std::memory_order_relaxed);
}

}  // namespace dusk::net
