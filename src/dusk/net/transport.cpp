#include "dusk/net/transport.h"

#include <aurora/lib/logging.hpp>

#include <chrono>

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
    if (host_ != nullptr) {
        NetLog.warn("start host called on an already-running transport");
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
        return false;
    }
    host_ = host;
    stop_.store(false, std::memory_order_relaxed);
    peerSlots_.fill(nullptr);
    thread_ = std::thread(&Transport::SocketThreadMain, this);
    NetLog.info("host transport up on port {}", host_->address.port);
    return true;
}

bool Transport::StartClient(const std::string& host, u16 port) {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (host_ != nullptr) {
        NetLog.warn("start client called on an already-running transport");
        return false;
    }

    ENetHost* clientHost = enet_host_create(nullptr, /*peerCount=*/1, /*channelLimit=*/2, 0, 0);
    if (clientHost == nullptr) {
        NetLog.error("enet_host_create failed for client");
        return false;
    }

    ENetAddress address;
    if (enet_address_set_host(&address, host.c_str()) != 0) {
        NetLog.error("enet_address_set_host failed for '{}'", host);
        enet_host_destroy(clientHost);
        return false;
    }
    address.port = port;

    ENetPeer* peer = enet_host_connect(clientHost, &address, /*channelCount=*/2, 0);
    if (peer == nullptr) {
        NetLog.error("enet_host_connect failed for {}:{}", host, port);
        enet_host_destroy(clientHost);
        return false;
    }

    host_ = clientHost;
    stop_.store(false, std::memory_order_relaxed);
    peerSlots_.fill(nullptr);
    // The client's single peer is not yet "connected"; the CONNECT event on
    // the socket thread assigns it slot 0.
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
    if (host_ == nullptr || size > kMaxMessageSize) {
        return false;
    }
    OutboundPacket p;
    p.peerIndex = peerIndex;
    p.channel = channel;
    p.size = size;
    std::memcpy(p.data, data, size);
    if (!outbox_.Push(p)) {
        outboundDropped_.fetch_add(1, std::memory_order_relaxed);
        NetLog.warn("outbox ring full; dropping {} bytes to peer {}", size, peerIndex);
        return false;
    }
    return true;
}

bool Transport::Poll(InboundPacket& out) {
    return inbox_.Pop(out);
}

// ---------------------------------------------------------------------------
// Socket thread
// ---------------------------------------------------------------------------

void Transport::SocketThreadMain() {
    running_.store(true, std::memory_order_relaxed);
    while (!stop_.load(std::memory_order_relaxed)) {
        ENetEvent event;
        while (enet_host_service(host_, &event, kServiceTimeoutMs) > 0) {
            switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                const u8 index = AssignPeerSlot(event.peer);
                if (index == kInvalidPlayerId) {
                    NetLog.warn("peer limit reached; resetting incoming connection");
                    enet_peer_reset(event.peer);
                    break;
                }
                InboundPacket pkt;
                pkt.type = NetEventType::Connected;
                pkt.peerIndex = index;
                pkt.size = 0;
                if (!inbox_.Push(pkt)) {
                    inboundDropped_.fetch_add(1, std::memory_order_relaxed);
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
                if (!inbox_.Push(pkt)) {
                    inboundDropped_.fetch_add(1, std::memory_order_relaxed);
                }
                NetLog.info("peer {} disconnected", index);
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                const u8 index = PeerSlot(event.peer);
                if (index != kInvalidPlayerId && event.packet->dataLength <= kMaxMessageSize) {
                    InboundPacket pkt;
                    pkt.type = NetEventType::Data;
                    pkt.peerIndex = index;
                    pkt.channel = static_cast<u8>(event.channelID);
                    pkt.size = static_cast<u16>(event.packet->dataLength);
                    std::memcpy(pkt.data, event.packet->data, pkt.size);
                    if (!inbox_.Push(pkt)) {
                        inboundDropped_.fetch_add(1, std::memory_order_relaxed);
                        NetLog.warn("inbox ring full; dropping {} bytes from peer {}", pkt.size, index);
                    } else {
                        packetsReceived_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                enet_packet_destroy(event.packet);
                break;
            }
            case ENET_EVENT_TYPE_NONE:
                break;
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
    running_.store(false, std::memory_order_relaxed);
    NetLog.info("transport socket thread exited");
}

void Transport::DrainOutbox() {
    OutboundPacket p;
    while (outbox_.Pop(p)) {
        ENetPeer* peer = PeerAt(p.peerIndex);
        if (peer == nullptr) {
            continue;  // peer vanished between enqueue and drain; drop silently
        }
        const enet_uint32 flags =
            (p.channel == kChannelReliable) ? ENET_PACKET_FLAG_RELIABLE : 0;
        ENetPacket* packet = enet_packet_create(p.data, p.size, flags);
        if (packet == nullptr) {
            NetLog.warn("enet_packet_create failed for {} bytes", p.size);
            continue;
        }
        if (enet_peer_send(peer, p.channel, packet) < 0) {
            enet_packet_destroy(packet);
            NetLog.warn("enet_peer_send failed for peer {}", p.peerIndex);
            continue;
        }
        packetsSent_.fetch_add(1, std::memory_order_relaxed);
    }
    enet_host_flush(host_);
}

// ---------------------------------------------------------------------------
// Peer slot management (socket thread only)
// ---------------------------------------------------------------------------

u8 Transport::AssignPeerSlot(ENetPeer* peer) {
    for (size_t i = 0; i < kMaxPeers; ++i) {
        if (peerSlots_[i] == nullptr) {
            peerSlots_[i] = peer;
            return static_cast<u8>(i);
        }
    }
    return kInvalidPlayerId;
}

void Transport::ReleasePeerSlot(u8 index) {
    if (index < kMaxPeers) {
        peerSlots_[index] = nullptr;
    }
}

u8 Transport::PeerSlot(ENetPeer* peer) const {
    for (size_t i = 0; i < kMaxPeers; ++i) {
        if (peerSlots_[i] == peer) {
            return static_cast<u8>(i);
        }
    }
    return kInvalidPlayerId;
}

ENetPeer* Transport::PeerAt(u8 index) const {
    if (index >= kMaxPeers) {
        return nullptr;
    }
    return peerSlots_[index];
}

}  // namespace dusk::net
