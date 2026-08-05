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

// -------------------------------------------------------------------------
// Star-relay policy (MINOR d): how the host should route a game message it
// received from a peer. Today (M1) the host relays player state/events to
// every other joined peer. This seam exists so M2's per-type routing does
// NOT extend the current "game message = relay to all" fallthrough:
//   - CombatIntent (client -> sim owner): must NOT be relayed to other
//     clients — the owner validates and emits a CombatResult.
//   - EnemySnapshot: flows owner -> clients only (host->all); never
//     client->host then relayed.
//   - CombatResult / EnemyEvent: host simulcast (relay).
// The enum + switch below keep that split explicit without implementing M2.
// -------------------------------------------------------------------------
enum class RelayPolicy : u8 {
    None,  // not star-relayed (M2+ routed explicitly, not via the fallthrough)
    Star,  // relay to every joined peer except the origin (v1: PlayerState/PlayerEvent)
};

RelayPolicy PolicyFor(MsgType type) {
    switch (type) {
    case MsgType::PlayerState:
    case MsgType::PlayerEvent:
        return RelayPolicy::Star;
    default:
        // Handshake (Join*/WorldInit/PlayerLeave/SessionEnd) is handled
        // directly by the session, never via ForwardGameMessage. M2 game
        // traffic (EnemySnapshot/CombatIntent/CombatResult/EnemyEvent/...)
        // gets its own explicit dispatch at its seam, not the Star default.
        return RelayPolicy::None;
    }
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
    // Idempotent by guard (GLM MINOR-6): a never-started or already-stopped
    // session has nothing to tear down.
    if (state_ == SessionState::Idle || state_ == SessionState::Ended) {
        return;
    }
    if (role_ == SessionRole::Host && state_ == SessionState::Listening) {
        PayloadUnion payload = {};
        payload.sessionEnd.reason = static_cast<u8>(SessionEndReason::HostLeft);
        SendToAll(MsgType::SessionEnd, payload);
        NetLog.info("net: host stopping, sent SessionEnd to {} player(s)",
            static_cast<u32>(PresentCount()));
    } else if (role_ == SessionRole::Client && state_ == SessionState::Joined) {
        PayloadUnion payload = {};
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

    // Reliable overflow is an explicit failure, never a silent drop (deepseek
    // M2): surface the transport counters so a dropped control/event message
    // is visible at the session layer.
    const u64 roDropped = transport_.ReliableOutboundDropped();
    if (roDropped != lastReliableOutboundDropped_) {
        NetLog.warn("net: {} reliable outbound message(s) dropped (reliable outbox full)",
            roDropped - lastReliableOutboundDropped_);
        lastReliableOutboundDropped_ = roDropped;
    }
    const u64 riDropped = transport_.ReliableInboundDropped();
    if (riDropped != lastReliableInboundDropped_) {
        NetLog.warn("net: {} reliable inbound message(s) dropped (reliable inbox full)",
            riDropped - lastReliableInboundDropped_);
        lastReliableInboundDropped_ = riDropped;
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
        PayloadUnion payload = {};
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
    case MsgType::PlayerState:
    case MsgType::PlayerEvent:
        // M1 game traffic: the session owns transport/roster only. The game
        // side consumes the message; on the host the message is then relayed
        // to every other joined peer (star topology, 00-network.md §2).
        if (gameHandler_) {
            gameHandler_(msg.type, msg.payload);
        }
        if (role_ == SessionRole::Host) {
            ForwardGameMessage(pkt.peerIndex, msg.type, msg.payload);
        }
        break;
    default:
        // M0: snapshot/combat/time traffic is parsed but not acted on — no
        // game integration yet. The serializer round-trip is covered by the
        // selftest; application lands in M2/M3.
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
    // Duplicate JoinRequest guard (deepseek m2): a peer that already holds a
    // PlayerId must not get a second slot — ignore the re-join (a buggy or
    // malicious client would otherwise orphan the first assignment).
    if (peerToPlayer_[peerIndex] != kInvalidPlayerId) {
        NetLog.warn("net: duplicate JoinRequest from peer {} (already player {}); ignoring",
            peerIndex, peerToPlayer_[peerIndex]);
        return;
    }
    const auto& req = msg.payload.joinRequest;

    if (req.version != kProtocolVersion) {
        PayloadUnion payload = {};
        payload.joinReject.reason = static_cast<u8>(JoinRejectReason::VersionMismatch);
        SendToPeer(peerIndex, MsgType::JoinReject, payload);
        NetLog.warn("net: rejecting peer {} (version {} != {})", peerIndex, req.version,
            kProtocolVersion);
        return;
    }

    if (req.requestedSlot != kAnySlot &&
        (req.requestedSlot >= config_.maxPlayers || roster_[req.requestedSlot].present))
    {
        PayloadUnion payload = {};
        payload.joinReject.reason = static_cast<u8>(JoinRejectReason::InvalidSlot);
        SendToPeer(peerIndex, MsgType::JoinReject, payload);
        NetLog.warn("net: rejecting peer {} (slot {} unavailable)", peerIndex, req.requestedSlot);
        return;
    }

    if (PresentCount() >= config_.maxPlayers) {
        PayloadUnion payload = {};
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
    PayloadUnion accept = {};
    accept.joinAccept.assignedPlayerId = id;
    accept.joinAccept.stage = worldStage_;
    accept.joinAccept.time = worldTime_;
    accept.joinAccept.weather = worldWeather_;
    FillWireRoster(accept.joinAccept.roster);
    SendToPeer(peerIndex, MsgType::JoinAccept, accept);

    // WorldInit: fixed stage + roster (00-network.md §4/§5 — no state
    // sections); M1's snapshot-on-join is a burst of ordinary per-frame
    // PlayerState/EnemySnapshot messages sent right after this.
    PayloadUnion init = {};
    init.worldInit.stage = worldStage_;
    FillWireRoster(init.worldInit.roster);
    SendToPeer(peerIndex, MsgType::WorldInit, init);

    // Roster-refresh broadcast (MAJOR M1): every already-joined peer must
    // learn about the new player, or it will never spawn a puppet for them
    // (the game-side spawn gate is roster[pid].present) and never send them
    // PlayerState (RemoteInOurRoom skips non-present slots) — 3+ players
    // would be invisible to earlier joiners. WorldInit doubles as a
    // roster-refresh: it is fixed stage+roster with no state sections, and
    // the joining peer's pose arrives on the ordinary per-frame stream. The
    // new player itself already got JoinAccept/WorldInit, so it is excluded.
    SendToAll(MsgType::WorldInit, init, /*exceptPlayer=*/id);
}

void Session::OnJoinAccept(const Message& msg) {
    const auto& accept = msg.payload.joinAccept;
    // Semantic validation on receive (deepseek M5): never store an out-of-
    // range assigned id, and require the roster to back the assignment (the
    // host always sends both; anything else is a buggy/forged accept).
    if (accept.assignedPlayerId >= kMaxLocalPlayers) {
        NetLog.warn("net: JoinAccept assigns invalid player id {}; rejecting",
            accept.assignedPlayerId);
        rejectReasonName_ = "invalid join accept";
        state_ = SessionState::Rejected;
        return;
    }
    if (accept.roster[accept.assignedPlayerId].present == 0) {
        NetLog.warn("net: JoinAccept roster does not mark player {} present; rejecting",
            accept.assignedPlayerId);
        rejectReasonName_ = "invalid join accept";
        state_ = SessionState::Rejected;
        return;
    }
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
    NetLog.info("net: world init: stage '{}' room {} (players {})", init.stage.stage,
        static_cast<s32>(init.stage.room), init.roster.size());
}

// ---------------------------------------------------------------------------
// Game-message routing (star topology)
// ---------------------------------------------------------------------------

bool Session::SendGameMessage(MsgType type, const PayloadUnion& payload) {
    if (state_ != SessionState::Listening && state_ != SessionState::Joined) {
        return false;
    }
    if (role_ == SessionRole::Host) {
        // Broadcast to every joined peer. SendToAll skips non-present roster
        // slots, so this never echoes back to a peer that is still joining.
        SendToAll(type, payload);
    } else {
        // Client: everything flows to the host, which relays to the others.
        SendToPeer(0, type, payload);
    }
    return true;
}

void Session::ForwardGameMessage(u8 originPeer, MsgType type, const PayloadUnion& payload) {
    // Star-relay policy seam (MINOR d): see PolicyFor. M1 relays player
    // state/events; M2's CombatIntent/EnemySnapshot get explicit policies.
    if (PolicyFor(type) != RelayPolicy::Star) {
        return;
    }
    for (u8 peer = 0; peer < Transport::kMaxPeers; ++peer) {
        if (peer == originPeer) {
            continue;
        }
        // Only relay to peers that completed the join handshake (have a
        // PlayerId); a connected-but-joining peer gets the roster via
        // WorldInit and then the snapshot burst.
        if (peerToPlayer_[peer] != kInvalidPlayerId) {
            SendToPeer(peer, type, payload);
        }
    }
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
        PayloadUnion payload = {};
        payload.playerLeave.playerId = playerId;
        SendToAll(MsgType::PlayerLeave, payload, /*exceptPlayer=*/playerId);
    }
}

void Session::ApplyRoster(const std::array<PlayerInfo, kMaxLocalPlayers>& wireRoster) {
    roster_ = {};
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        const auto& entry = wireRoster[i];
        // Range-check the advertised player id too (deepseek M5): a roster
        // entry pointing outside the player-id space is ignored.
        if (entry.present != 0 && entry.playerId < kMaxLocalPlayers) {
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
