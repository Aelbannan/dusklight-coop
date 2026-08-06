#include "dusk/net/transport.h"

#include <aurora/lib/logging.hpp>

#include <chrono>
#include <cstring>

namespace dusk::net {

namespace {
aurora::Module NetLog("dusk::net::transport");

constexpr int kServiceTimeoutMs = 8;  // upper bound on socket-thread wakeup latency

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
        // Snapshots: replace-newest under pressure — Send never fails.
        if (snapshotOutbox_.PushOrReplace(p)) {
            snapshotOutboundReplaced_.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }
    // Reliable: a full ring is an explicit, visible failure, never a silent
    // drop (deepseek M2 / glm MAJOR-1). The counter is the session-visible
    // error signal.
    if (!reliableOutbox_.Push(p)) {
        reliableOutboundDropped_.fetch_add(1, std::memory_order_relaxed);
        NetLog.warn("reliable outbox ring full; dropping {} bytes to peer {} (explicit failure)",
            size, peerIndex);
        return false;
    }
    return true;
}

bool Transport::Poll(InboundPacket& out) {
    // Reliable inbox first (control/events keep priority), then snapshots.
    for (;;) {
        if (!reliableInbox_.Pop(out)) {
            break;
        }
        if (out.type == NetEventType::Data && out.generation != PeerGeneration(out.peerIndex)) {
            inboundGenerationDropped_.fetch_add(1, std::memory_order_relaxed);
            continue;  // slot reused by a newer connection; drop the stale packet
        }
        return true;
    }
    for (;;) {
        if (!snapshotInbox_.Pop(out)) {
            break;
        }
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
        if (!reliableInbox_.Push(pkt)) {
            reliableInboundDropped_.fetch_add(1, std::memory_order_relaxed);
            NetLog.warn("reliable inbox full; dropping connect event for peer {}", index);
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
        if (!reliableInbox_.Push(pkt)) {
            reliableInboundDropped_.fetch_add(1, std::memory_order_relaxed);
            NetLog.warn("reliable inbox full; dropping disconnect event for peer {}", index);
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
            if (snapshotInbox_.PushOrReplace(pkt)) {
                snapshotInboundReplaced_.fetch_add(1, std::memory_order_relaxed);
            }
            packetsReceived_.fetch_add(1, std::memory_order_relaxed);
        } else if (reliableInbox_.Push(pkt)) {
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
    // Reliable (control/events/combat) first, then snapshots.
    OutboundPacket p;
    while (reliableOutbox_.Pop(p)) {
        SendPacket(p);
    }
    while (snapshotOutbox_.Pop(p)) {
        SendPacket(p);
    }
    if (host_ != nullptr) {
        enet_host_flush(host_);
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
        peerGenerations_[index].fetch_add(1, std::memory_order_relaxed);
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
