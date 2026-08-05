#include "dusk/net/session.h"

#include <aurora/lib/logging.hpp>

#include <chrono>
#include <cstring>

namespace dusk::net {

namespace {

aurora::Module NetLog("dusk::net::session");

u64 NowMs() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void CopyName(char (&dst)[kMaxNameLength], const char* src) {
    std::strncpy(dst, src ? src : "", kMaxNameLength - 1);
    dst[kMaxNameLength - 1] = '\0';
}

}  // namespace

Session::~Session() {
    Stop();
}

bool Session::StartHost(const SessionConfig& config) {
    Stop();
    config_ = config;
    if (config_.maxPlayers == 0 || config_.maxPlayers > kMaxLocalPlayers) {
        config_.maxPlayers = kMaxLocalPlayers;
    }
    role_ = SessionRole::Host;
    state_ = SessionState::Idle;
    selfId_ = 0;
    rejectReasonName_ = "";
    frame_ = 0;
    connectedAtMs_ = 0;
    roster_ = {};
    peerToPlayer_.fill(kInvalidPlayerId);
    worldStage_ = StageInfo{};
    worldTime_ = TimeInfo{};
    worldWeather_ = WeatherInfo{};

    if (!transport_.StartHost(config_.port)) {
        NetLog.error("net: failed to start host transport on port {}", config_.port);
        role_ = SessionRole::None;
        return false;
    }

    // The host is player 0 from the moment it starts listening.
    roster_[0].playerId = 0;
    roster_[0].present = true;
    CopyName(roster_[0].name, config_.name.c_str());

    // Seed the world info sent to joiners (M1+ replaces this from the sim).
    worldStage_ = config_.stage;

    state_ = SessionState::Listening;
    NetLog.info("net: hosting '{}' on port {} (maxPlayers {})", config_.name,
        transport_.BoundPort(), static_cast<u32>(config_.maxPlayers));
    return true;
}

bool Session::StartClient(const SessionConfig& config) {
    Stop();
    config_ = config;
    if (config_.joinHost.empty()) {
        config_.joinHost = "127.0.0.1";
    }
    role_ = SessionRole::Client;
    state_ = SessionState::Idle;
    selfId_ = kInvalidPlayerId;
    rejectReasonName_ = "";
    frame_ = 0;
    connectedAtMs_ = 0;
    roster_ = {};
    peerToPlayer_.fill(kInvalidPlayerId);
    worldStage_ = StageInfo{};
    worldTime_ = TimeInfo{};
    worldWeather_ = WeatherInfo{};

    if (!transport_.StartClient(config_.joinHost, config_.port)) {
        NetLog.error("net: failed to start client transport to {}:{}", config_.joinHost, config_.port);
        role_ = SessionRole::None;
        return false;
    }

    state_ = SessionState::Connecting;
    NetLog.info("net: connecting to {}:{} as '{}'", config_.joinHost, config_.port, config_.name);
    return true;
}

void Session::Stop() {
    if (role_ == SessionRole::None && state_ == SessionState::Idle) {
        return;  // never started
    }
    if (role_ == SessionRole::Host && state_ == SessionState::Listening) {
        PayloadUnion payload;
        payload.sessionEnd.reason = static_cast<u8>(SessionEndReason::HostLeft);
        SendToAll(MsgType::SessionEnd, payload);
        NetLog.info("net: host stopping, sent SessionEnd to {} player(s)",
            static_cast<u32>(PresentCount()));
    } else if (role_ == SessionRole::Client && state_ == SessionState::Joined) {
        PayloadUnion payload;
        payload.playerLeave.playerId = selfId_;
        SendToPeer(0, MsgType::PlayerLeave, payload);
        NetLog.info("net: client stopping, sent PlayerLeave for player {}", selfId_);
    }
    transport_.Stop();
    state_ = SessionState::Ended;
    NetLog.info("net: session stopped");
}

void Session::Update() {
    if (state_ == SessionState::Idle || state_ == SessionState::Ended) {
        return;
    }
    ++frame_;

    InboundPacket pkt;
    while (transport_.Poll(pkt)) {
        switch (pkt.type) {
        case NetEventType::Connected:
            HandleConnect(pkt.peerIndex);
            break;
        case NetEventType::Disconnected:
            HandleDisconnect(pkt.peerIndex);
            break;
        case NetEventType::Data:
            HandleData(pkt);
            break;
        }
    }

    // Client join deadline: Connected must turn into Joined in time.
    if (role_ == SessionRole::Client && state_ == SessionState::Connected && connectedAtMs_ != 0 &&
        NowMs() - connectedAtMs_ >= config_.joinTimeoutMs)
    {
        rejectReasonName_ = "join timed out";
        state_ = SessionState::Rejected;
        NetLog.warn("net: join timed out after {} ms", config_.joinTimeoutMs);
    }
}

// ---------------------------------------------------------------------------
// Transport event handlers
// ---------------------------------------------------------------------------

void Session::HandleConnect(u8 peerIndex) {
    if (role_ == SessionRole::Host) {
        NetLog.info("net: peer {} connected, awaiting JoinRequest", peerIndex);
        return;
    }
    // Client: slot 0 is our connection to the host.
    if (state_ == SessionState::Connecting) {
        state_ = SessionState::Connected;
        connectedAtMs_ = NowMs();
        PayloadUnion payload;
        payload.joinRequest.version = config_.version;
        payload.joinRequest.requestedSlot = config_.requestedSlot;
        CopyName(payload.joinRequest.name, config_.name.c_str());
        SendToPeer(0, MsgType::JoinRequest, payload);
        NetLog.info("net: sent JoinRequest (version {})", config_.version);
    }
}

void Session::HandleDisconnect(u8 peerIndex) {
    if (role_ == SessionRole::Host) {
        if (peerIndex < Transport::kMaxPeers) {
            const PlayerId pid = peerToPlayer_[peerIndex];
            if (pid != kInvalidPlayerId) {
                RemovePlayer(pid, /*broadcastLeave=*/true);
            }
            peerToPlayer_[peerIndex] = kInvalidPlayerId;
        }
        return;
    }
    // Client: the host went away.
    NetLog.warn("net: connection to host lost");
    state_ = SessionState::Ended;
}

void Session::HandleData(const InboundPacket& pkt) {
    if (pkt.size < 4) {
        NetLog.warn("net: undersized packet from peer {}", pkt.peerIndex);
        return;
    }
    ByteReader reader(pkt.data, pkt.size);
    Message msg;
    if (!DeserializeMessage(reader, msg)) {
        NetLog.warn("net: malformed packet from peer {} ({} bytes)", pkt.peerIndex, pkt.size);
        return;
    }
    switch (msg.type) {
    case MsgType::JoinRequest:
        if (role_ == SessionRole::Host) {
            OnJoinRequest(pkt.peerIndex, msg);
        }
        break;
    case MsgType::JoinAccept:
        if (role_ == SessionRole::Client) {
            OnJoinAccept(msg);
        }
        break;
    case MsgType::JoinReject:
        if (role_ == SessionRole::Client) {
            OnJoinReject(msg);
        }
        break;
    case MsgType::PlayerLeave:
        OnPlayerLeave(msg);
        break;
    case MsgType::SessionEnd:
        if (role_ == SessionRole::Client) {
            OnSessionEnd(msg);
        }
        break;
    case MsgType::WorldInit:
        if (role_ == SessionRole::Client) {
            OnWorldInit(msg);
        }
        break;
    default:
        // M0: snapshot/combat/time traffic is parsed but not acted on — no
        // game integration yet. The serializer round-trip is covered by the
        // selftest; application lands in M1/M2/M3.
        NetLog.debug("net: ignoring {} for now (M0)", static_cast<u16>(msg.type));
        break;
    }
}

// ---------------------------------------------------------------------------
// Message handlers
// ---------------------------------------------------------------------------

void Session::OnJoinRequest(u8 peerIndex, const Message& msg) {
    if (peerIndex >= Transport::kMaxPeers) {
        return;
    }
    const auto& req = msg.payload.joinRequest;

    if (req.version != kProtocolVersion) {
        PayloadUnion payload;
        payload.joinReject.reason = static_cast<u8>(JoinRejectReason::VersionMismatch);
        SendToPeer(peerIndex, MsgType::JoinReject, payload);
        NetLog.warn("net: rejecting peer {} (version {} != {})", peerIndex, req.version,
            kProtocolVersion);
        return;
    }

    if (req.requestedSlot != kAnySlot &&
        (req.requestedSlot >= config_.maxPlayers || roster_[req.requestedSlot].present))
    {
        PayloadUnion payload;
        payload.joinReject.reason = static_cast<u8>(JoinRejectReason::InvalidSlot);
        SendToPeer(peerIndex, MsgType::JoinReject, payload);
        NetLog.warn("net: rejecting peer {} (slot {} unavailable)", peerIndex, req.requestedSlot);
        return;
    }

    if (PresentCount() >= config_.maxPlayers) {
        PayloadUnion payload;
        payload.joinReject.reason = static_cast<u8>(JoinRejectReason::SessionFull);
        SendToPeer(peerIndex, MsgType::JoinReject, payload);
        NetLog.warn("net: rejecting peer {} (session full, {}/{} players)", peerIndex,
            static_cast<u32>(PresentCount()), static_cast<u32>(config_.maxPlayers));
        return;
    }

    const PlayerId id = AssignPlayerId(req.requestedSlot);
    roster_[id].playerId = id;
    roster_[id].present = true;
    CopyName(roster_[id].name, req.name);
    peerToPlayer_[peerIndex] = id;
    NetLog.info("net: player {} '{}' joined (peer {})", id, roster_[id].name, peerIndex);

    // JoinAccept: assigned id + full roster + world info (00-network.md §4).
    PayloadUnion accept;
    accept.joinAccept.assignedPlayerId = id;
    accept.joinAccept.stage = worldStage_;
    accept.joinAccept.time = worldTime_;
    accept.joinAccept.weather = worldWeather_;
    FillWireRoster(accept.joinAccept.roster);
    SendToPeer(peerIndex, MsgType::JoinAccept, accept);

    // WorldInit: stage + roster; player/enemy state sections carry 0 in M0.
    PayloadUnion init;
    init.worldInit.stage = worldStage_;
    init.worldInit.playerStateCount = 0;
    init.worldInit.enemyStateCount = 0;
    FillWireRoster(init.worldInit.roster);
    SendToPeer(peerIndex, MsgType::WorldInit, init);
}

void Session::OnJoinAccept(const Message& msg) {
    const auto& accept = msg.payload.joinAccept;
    selfId_ = accept.assignedPlayerId;
    worldStage_ = accept.stage;
    worldTime_ = accept.time;
    worldWeather_ = accept.weather;
    ApplyRoster(accept.roster);
    state_ = SessionState::Joined;
    NetLog.info("net: joined as player {}", selfId_);
}

void Session::OnJoinReject(const Message& msg) {
    switch (static_cast<JoinRejectReason>(msg.payload.joinReject.reason)) {
    case JoinRejectReason::SessionFull:
        rejectReasonName_ = "session full";
        break;
    case JoinRejectReason::VersionMismatch:
        rejectReasonName_ = "version mismatch";
        break;
    case JoinRejectReason::InvalidSlot:
        rejectReasonName_ = "invalid slot";
        break;
    default:
        rejectReasonName_ = "rejected";
        break;
    }
    NetLog.warn("net: join rejected: {}", rejectReasonName_);
    state_ = SessionState::Rejected;
}

void Session::OnPlayerLeave(const Message& msg) {
    const u8 playerId = msg.payload.playerLeave.playerId;
    if (playerId >= kMaxLocalPlayers) {
        return;
    }
    if (role_ == SessionRole::Host) {
        // Relay the leave to the remaining players (00-network.md §4).
        RemovePlayer(playerId, /*broadcastLeave=*/true);
        return;
    }
    if (roster_[playerId].present) {
        NetLog.info("net: player {} left", playerId);
    }
    roster_[playerId].present = false;
    roster_[playerId].playerId = kInvalidPlayerId;
    roster_[playerId].name[0] = '\0';
}

void Session::OnSessionEnd(const Message& msg) {
    NetLog.info("net: host ended the session (reason {})", msg.payload.sessionEnd.reason);
    state_ = SessionState::Ended;
}

void Session::OnWorldInit(const Message& msg) {
    const auto& init = msg.payload.worldInit;
    worldStage_ = init.stage;
    ApplyRoster(init.roster);  // roster refresh; may include players who joined later
    NetLog.info("net: world init: stage '{}' room {} (players {}, enemies {})", init.stage.stage,
        static_cast<s32>(init.stage.room), init.playerStateCount, init.enemyStateCount);
}

// ---------------------------------------------------------------------------
// Send helpers
// ---------------------------------------------------------------------------

void Session::SendToPeer(u8 peerIndex, MsgType type, const PayloadUnion& payload) {
    if (peerIndex >= Transport::kMaxPeers) {
        NetLog.warn("net: send to invalid peer index {}", peerIndex);
        return;
    }
    u8 buf[kMaxMessageSize];
    ByteWriter writer(buf, sizeof(buf));
    Message msg;
    msg.type = type;
    msg.payload = payload;
    if (!SerializeMessage(msg, writer)) {
        NetLog.warn("net: failed to serialize message {}", static_cast<u16>(type));
        return;
    }
    if (!transport_.Send(peerIndex, ChannelFor(type), buf, writer.size())) {
        NetLog.warn("net: dropped {} to peer {} (transport send failed)", static_cast<u16>(type),
            peerIndex);
    }
}

void Session::SendToAll(MsgType type, const PayloadUnion& payload, u8 exceptPlayer) {
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        if (i == exceptPlayer || !roster_[i].present) {
            continue;
        }
        const u8 peer = PlayerPeer(i);
        if (peer != kInvalidPlayerId) {
            SendToPeer(peer, type, payload);
        }
    }
}

// ---------------------------------------------------------------------------
// Host logic
// ---------------------------------------------------------------------------

PlayerId Session::AssignPlayerId(u8 requestedSlot) {
    if (requestedSlot != kAnySlot && requestedSlot < config_.maxPlayers && !roster_[requestedSlot].present) {
        return requestedSlot;
    }
    for (u8 i = 0; i < config_.maxPlayers; ++i) {
        if (!roster_[i].present) {
            return i;
        }
    }
    return kInvalidPlayerId;  // unreachable: caller checked capacity
}

u8 Session::PresentCount() const {
    u8 count = 0;
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        if (roster_[i].present) {
            ++count;
        }
    }
    return count;
}

u8 Session::PlayerPeer(u8 playerId) const {
    for (u8 i = 0; i < Transport::kMaxPeers; ++i) {
        if (peerToPlayer_[i] == playerId) {
            return i;
        }
    }
    return kInvalidPlayerId;
}

void Session::RemovePlayer(u8 playerId, bool broadcastLeave) {
    if (playerId >= kMaxLocalPlayers || !roster_[playerId].present) {
        return;
    }
    roster_[playerId].present = false;
    roster_[playerId].playerId = kInvalidPlayerId;
    roster_[playerId].name[0] = '\0';
    NetLog.info("net: player {} left the session", playerId);
    if (broadcastLeave) {
        PayloadUnion payload;
        payload.playerLeave.playerId = playerId;
        SendToAll(MsgType::PlayerLeave, payload, /*exceptPlayer=*/playerId);
    }
}

void Session::ApplyRoster(const std::array<PlayerInfo, kMaxLocalPlayers>& wireRoster) {
    roster_ = {};
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        const auto& entry = wireRoster[i];
        if (entry.present != 0) {
            roster_[i].playerId = entry.playerId;
            roster_[i].present = true;
            CopyName(roster_[i].name, entry.name);
        }
    }
}

void Session::FillWireRoster(std::array<PlayerInfo, kMaxLocalPlayers>& out) const {
    out = {};
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        const auto& slot = roster_[i];
        if (!slot.present) {
            continue;
        }
        auto& entry = out[i];
        entry.playerId = slot.playerId;
        entry.present = 1;
        entry.state = static_cast<u8>(PlayerStateId::Connected);
        std::strncpy(entry.name, slot.name, kMaxNameLength - 1);
        entry.name[kMaxNameLength - 1] = '\0';
    }
}

}  // namespace dusk::net
