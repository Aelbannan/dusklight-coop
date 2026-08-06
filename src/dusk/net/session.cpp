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
// Star-relay policy (MINOR d, extended M4): how the host should route a game
// message it received from a peer. M1 relays player state/events to every
// other joined peer. M2 kept enemy/combat traffic OFF the fallthrough (the
// sim owner routes it explicitly). M4 (room ownership) widens the seam:
//   - CombatIntent (client -> ROOM owner): the host routes it to the room's
//     owner peer (RouteCombatIntent) — never relayed to other clients.
//   - EnemySnapshot (room owner -> the room): inbound copies from a client
//     owner are relayed to peers in the SENDER's room only (RoomScoped); the
//     host's own snapshots fan out the same way via SendGameMessage.
//   - CombatResult / EnemyEvent (owner -> all): the room owner may be a
//     CLIENT now, so an inbound copy IS re-broadcast to every other joined
//     peer (receivers no-op for rooms/ids they do not have).
//   - TimeSync / TimeEvent / WeatherChange: host-generated host->all, never
//     relayed (unchanged).
// -------------------------------------------------------------------------
enum class RelayPolicy : u8 {
    None,       // not star-relayed (handshake, time/weather; CombatIntent is
                //   routed explicitly, see RouteCombatIntent)
    Star,       // relay to every joined peer except the origin (PlayerState /
                //   PlayerEvent; owner events CombatResult/EnemyEvent)
    RoomScoped, // relay to joined peers in the ORIGIN's room except the
                //   origin (EnemySnapshot, M4)
};

RelayPolicy PolicyFor(MsgType type) {
    switch (type) {
    case MsgType::PlayerState:
    case MsgType::PlayerEvent:
    case MsgType::CombatResult:
    case MsgType::EnemyEvent:
        return RelayPolicy::Star;
    case MsgType::EnemySnapshot:
        // M4: room-owner snapshots reach the room's other players only; a
        // client in another room has no local instance to apply them to
        // (per-room actors) and the sender gate already skips them. The host
        // relays to peers whose last-known room matches the SENDER's.
        return RelayPolicy::RoomScoped;
    case MsgType::CombatIntent:
        // M4: routed explicitly to the room's owner (RouteCombatIntent);
        // never star-relayed.
        return RelayPolicy::None;
    case MsgType::TimeSync:
    case MsgType::TimeEvent:
    case MsgType::WeatherChange:
        // M3: the host owns one clock and one sky (00-network.md §8); a peer
        // that sends these is reported into the handler (for diagnostics) and
        // not echoed to anyone else.
        return RelayPolicy::None;
    default:
        // Handshake (Join*/WorldInit/PlayerLeave/SessionEnd/RoomOwnership) is
        // handled directly by the session, never via ForwardGameMessage.
        return RelayPolicy::None;
    }
}

}  // namespace

Session::~Session() {
    Stop();
}

RoomOwnershipTable::RoomState* RoomOwnershipTable::Find(const RoomKey& key) {
    for (auto& rs : rooms_) {
        if (rs.key == key) {
            return &rs;
        }
    }
    return nullptr;
}

const RoomOwnershipTable::RoomState* RoomOwnershipTable::Find(const RoomKey& key) const {
    for (const auto& rs : rooms_) {
        if (rs.key == key) {
            return &rs;
        }
    }
    return nullptr;
}

void RoomOwnershipTable::RecomputeOwner(RoomState& rs, bool isHost,
                                        std::vector<RoomKey>* changed) {
    const u8 before = rs.owner;
    // The world host defaults to owning its own room (network.md §6); the
    // order array keeps the host's arrival, but the host's presence wins.
    rs.owner = kInvalidPlayerId;
    if (isHost) {
        for (u8 i = 0; i < rs.orderLen; ++i) {
            if (rs.order[i] == 0) {
                rs.owner = 0;
                break;
            }
        }
    }
    if (rs.owner == kInvalidPlayerId && rs.orderLen > 0) {
        rs.owner = rs.order[0];  // first arrival
    }
    if (rs.owner != before && changed != nullptr) {
        changed->push_back(rs.key);
    }
}

static void FillRoomKey(RoomKey& key, const char* stage, s8 room) {
    std::strncpy(key.stage, stage ? stage : "", kMaxStageNameLength - 1);
    key.stage[kMaxStageNameLength - 1] = '\0';
    key.room = room;
}

u8 RoomOwnershipTable::OnPlayerEnter(const char* stage, s8 room, u8 pid, bool isHost,
                                     std::vector<RoomKey>* changed) {
    if (room < 0 || pid >= kMaxLocalPlayers) {
        return kInvalidPlayerId;
    }
    RoomKey key;
    FillRoomKey(key, stage, room);
    RoomState* rs = Find(key);
    if (rs == nullptr) {
        rooms_.push_back({});
        rs = &rooms_.back();
        rs->key = key;
    }
    // Idempotent: the same pid re-announcing its room is a no-op.
    for (u8 i = 0; i < rs->orderLen; ++i) {
        if (rs->order[i] == pid) {
            RecomputeOwner(*rs, isHost, changed);
            return rs->owner;
        }
    }
    if (rs->orderLen < kMaxLocalPlayers) {
        rs->order[rs->orderLen++] = pid;
    }
    // Sticky rule: arrival never takes ownership from a current owner.
    // RecomputeOwner keeps the current owner unless the HOST just entered the
    // room (host-defaults-own-its-room) or the room was ownerless — no
    // ping-pong (network.md §6).
    RecomputeOwner(*rs, isHost, changed);
    return rs->owner;
}

void RoomOwnershipTable::OnPlayerLeave(const char* stage, s8 room, u8 pid, bool isHost,
                                       std::vector<RoomKey>* changed) {
    if (room < 0) {
        return;
    }
    RoomKey key;
    FillRoomKey(key, stage, room);
    RoomState* rs = Find(key);
    if (rs == nullptr) {
        return;
    }
    bool removed = false;
    for (u8 i = 0; i < rs->orderLen; ++i) {
        if (rs->order[i] == pid) {
            for (u8 j = i; j + 1 < rs->orderLen; ++j) {
                rs->order[j] = rs->order[j + 1];
            }
            --rs->orderLen;
            removed = true;
            break;
        }
    }
    if (!removed) {
        return;
    }
    RecomputeOwner(*rs, isHost, changed);  // transfer to next arrival / ownerless
    if (rs->orderLen == 0 && rs->owner == kInvalidPlayerId) {
        for (auto it = rooms_.begin(); it != rooms_.end(); ++it) {
            if (it->key == key) {
                rooms_.erase(it);
                break;
            }
        }
    }
}

void RoomOwnershipTable::OnPlayerDisconnect(u8 pid, bool isHost,
                                            std::vector<RoomKey>* changed) {
    if (pid >= kMaxLocalPlayers) {
        return;
    }
    std::vector<RoomKey> present;
    for (const auto& rs : rooms_) {
        for (u8 i = 0; i < rs.orderLen; ++i) {
            if (rs.order[i] == pid) {
                present.push_back(rs.key);
                break;
            }
        }
    }
    for (const auto& key : present) {
        OnPlayerLeave(key.stage, key.room, pid, isHost, changed);
    }
}

u8 RoomOwnershipTable::OwnerOf(const char* stage, s8 room) const {
    if (room < 0) {
        return kInvalidPlayerId;
    }
    RoomKey key;
    FillRoomKey(key, stage, room);
    const RoomState* rs = Find(key);
    return rs != nullptr ? rs->owner : kInvalidPlayerId;
}

void RoomOwnershipTable::OwnedRooms(std::vector<RoomKey>& out) const {
    for (const auto& rs : rooms_) {
        if (rs.owner != kInvalidPlayerId) {
            out.push_back(rs.key);
        }
    }
}

void RoomOwnershipTable::SetOwner(const char* stage, s8 room, u8 owner) {
    if (room < 0) {
        return;
    }
    RoomKey key;
    FillRoomKey(key, stage, room);
    RoomState* rs = Find(key);
    if (owner == kInvalidPlayerId) {
        if (rs != nullptr) {
            for (auto it = rooms_.begin(); it != rooms_.end(); ++it) {
                if (it->key == key) {
                    rooms_.erase(it);
                    break;
                }
            }
        }
        return;
    }
    if (rs == nullptr) {
        rooms_.push_back({});
        rs = &rooms_.back();
        rs->key = key;
    }
    rs->owner = owner;
    // Minimal membership view: the client does not track arrival order for
    // other players' rooms — OwnerOf is the only read it needs.
    rs->orderLen = 0;
    if (owner < kMaxLocalPlayers) {
        rs->order[rs->orderLen++] = owner;
    }
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
    endReason_ = SessionEndReason::Shutdown;
    frame_ = 0;
    connectedAtMs_ = 0;
    roster_ = {};
    peerToPlayer_.fill(kInvalidPlayerId);
    playerRoom_.fill(RoomKey{});
    ownership_.Clear();
    worldStage_ = StageInfo{};
    worldTime_ = TimeStateInfo{};
    worldWeather_ = WeatherStateInfo{};

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
    endReason_ = SessionEndReason::Shutdown;
    frame_ = 0;
    connectedAtMs_ = 0;
    roster_ = {};
    peerToPlayer_.fill(kInvalidPlayerId);
    playerRoom_.fill(RoomKey{});
    ownership_.Clear();
    worldStage_ = StageInfo{};
    worldTime_ = TimeStateInfo{};
    worldWeather_ = WeatherStateInfo{};

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
    // Client: the host went away (no SessionEnd arrived — ENet detected the
    // dead peer). Distinct end reason for the host-leave UX (M4 D8).
    NetLog.warn("net: connection to host lost");
    endReason_ = SessionEndReason::ConnectionLost;
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
        // (M4: per-type policy — PlayerState/PlayerEvent star, M2/M4 enemy
        // traffic per PolicyFor). M4: the host also sniffs the sender's room
        // from the stream to drive the room-ownership table.
        if (role_ == SessionRole::Host) {
            if (msg.type == MsgType::PlayerState) {
                const auto& st = msg.payload.playerState;
                if (st.playerId < kMaxLocalPlayers && roster_[st.playerId].present) {
                    UpdatePlayerRoom(st.playerId, st.stage, st.roomNo, /*isHost=*/false);
                }
            } else if (msg.type == MsgType::PlayerEvent) {
                const auto& ev = msg.payload.playerEvent;
                if (ev.playerId < kMaxLocalPlayers && roster_[ev.playerId].present &&
                    static_cast<PlayerEventId>(ev.eventId) == PlayerEventId::SceneChange)
                {
                    // The reliable SceneChange is the room-change authority
                    // (a dropped PlayerState can't lose the new room). The
                    // stage comes from the player's last-known room — skip
                    // until the first PlayerState established it (an empty
                    // stage here would key a bogus ("", room) entry).
                    if (playerRoom_[ev.playerId].stage[0] != '\0') {
                        UpdatePlayerRoom(ev.playerId, playerRoom_[ev.playerId].stage,
                            static_cast<s8>(ev.data & 0xFF), /*isHost=*/false);
                    }
                }
            }
        }
        if (gameHandler_) {
            gameHandler_(msg.type, msg.payload);
        }
        if (role_ == SessionRole::Host) {
            ForwardGameMessage(pkt.peerIndex, msg.type, msg.payload);
        }
        break;
    case MsgType::EnemySnapshot:
    case MsgType::EnemyEvent:
    case MsgType::CombatResult:
        // M2/M4 enemy/combat traffic. The game handler consumes it; the host
        // relays per PolicyFor (M4: a client room owner's EnemySnapshot is
        // room-scoped, its CombatResult/EnemyEvent are star-relayed). A
        // buggy/forged client's copies are never echoed to other clients.
        if (gameHandler_) {
            gameHandler_(msg.type, msg.payload);
        }
        if (role_ == SessionRole::Host) {
            ForwardGameMessage(pkt.peerIndex, msg.type, msg.payload);
        }
        break;
    case MsgType::CombatIntent:
        // M4 room ownership: intents are validated by the ROOM's owner, which
        // may be a client. The host routes to the owner peer; when the host
        // owns the room it consumes the intent itself (gameHandler). The
        // intent is never star-relayed (PolicyFor None).
        if (role_ == SessionRole::Host) {
            RouteCombatIntent(pkt.peerIndex, msg.payload);
        } else if (gameHandler_) {
            gameHandler_(msg.type, msg.payload);
        }
        break;
    case MsgType::RoomOwnership:
        if (role_ == SessionRole::Client) {
            OnRoomOwnership(msg);
        }
        break;
    case MsgType::TimeSync:
    case MsgType::TimeEvent:
    case MsgType::WeatherChange:
        // M3 time/weather traffic. Consumed by the game handler; NEVER
        // relayed (PolicyFor returns None) — the host generates these and
        // broadcasts them host->all via SendGameMessage; a client sending
        // them is a buggy/forged peer and is ignored rather than echoed.
        if (gameHandler_) {
            gameHandler_(msg.type, msg.payload);
        }
        break;
    default:
        // Handshake leftovers — parsed but not acted on here.
        NetLog.debug("net: ignoring {} (no handler)", static_cast<u16>(msg.type));
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

    // JoinAccept: assigned id + full roster + world info (00-network.md §4;
    // M3: time/weather so a mid-game joiner starts with the host's sky).
    PayloadUnion accept = {};
    accept.joinAccept.assignedPlayerId = id;
    accept.joinAccept.stage = worldStage_;
    accept.joinAccept.time = worldTime_;
    accept.joinAccept.weather = worldWeather_;
    FillWireRoster(accept.joinAccept.roster);
    SendToPeer(peerIndex, MsgType::JoinAccept, accept);

    // WorldInit: fixed stage + time + weather + roster (00-network.md
    // §4/§5); M1's snapshot-on-join is a burst of ordinary per-frame
    // PlayerState/EnemySnapshot messages sent right after this; M3 carries
    // the current sky (task 6).
    PayloadUnion init = {};
    init.worldInit.stage = worldStage_;
    init.worldInit.time = worldTime_;
    init.worldInit.weather = worldWeather_;
    FillWireRoster(init.worldInit.roster);
    SendToPeer(peerIndex, MsgType::WorldInit, init);

    // M4: the joining peer must know who owns which room (network.md §6) —
    // one RoomOwnershipMsg per owned room, right after WorldInit (reliable,
    // ordered). Without it a client would think it owns its own room when a
    // host or earlier player already does.
    SendOwnershipMap(peerIndex);

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
    endReason_ = SessionEndReason::Shutdown;
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
    endReason_ = static_cast<SessionEndReason>(msg.payload.sessionEnd.reason);
    state_ = SessionState::Ended;
}

void Session::OnWorldInit(const Message& msg) {
    const auto& init = msg.payload.worldInit;
    worldStage_ = init.stage;
    worldTime_ = init.time;
    worldWeather_ = init.weather;
    ApplyRoster(init.roster);  // roster refresh; may include players who joined later
    NetLog.info("net: world init: stage '{}' room {} (players {})", init.stage.stage,
        static_cast<s32>(init.stage.room), init.roster.size());
}

void Session::OnRoomOwnership(const Message& msg) {
    const auto& om = msg.payload.roomOwnership;
    // Client-side view: the host's assignment is authoritative (the sticky
    // first-in-room table lives on the host; the client only needs OwnerOf
    // to know whether IT owns its room).
    ownership_.SetOwner(om.stage, om.room, om.owner);
    NetLog.debug("net: room {}:{} owned by player {}", om.stage, static_cast<s32>(om.room),
        om.owner);
}

// ---------------------------------------------------------------------------
// Game-message routing (star topology)
// ---------------------------------------------------------------------------

bool Session::SendGameMessage(MsgType type, const PayloadUnion& payload) {
    if (state_ != SessionState::Listening && state_ != SessionState::Joined) {
        return false;
    }
    if (role_ == SessionRole::Host) {
        // M4 per-type fan-out: EnemySnapshot reaches the room's players only
        // (the host's own room); everything else goes to every joined peer.
        if (PolicyFor(type) == RelayPolicy::RoomScoped) {
            const RoomKey& room = playerRoom_[0];
            if (room.room >= 0) {
                SendToAllInRoom(room, type, payload, /*exceptPlayer=*/0);
            } else {
                SendToAll(type, payload, /*exceptPlayer=*/0);
            }
        } else {
            SendToAll(type, payload);
        }
    } else {
        // Client: everything flows to the host, which relays to the others.
        SendToPeer(0, type, payload);
    }
    return true;
}

void Session::ForwardGameMessage(u8 originPeer, MsgType type, const PayloadUnion& payload) {
    // Star-relay policy seam (MINOR d, extended M4): see PolicyFor.
    const RelayPolicy policy = PolicyFor(type);
    if (policy == RelayPolicy::None) {
        return;
    }
    if (policy == RelayPolicy::RoomScoped) {
        // M4: relay a client room owner's EnemySnapshot to peers in the
        // SENDER's room (its snapshots are about the room it owns = the room
        // it is in). Peers in other rooms have no local instances for them.
        const u8 originPid = peerToPlayer_[originPeer];
        if (originPid == kInvalidPlayerId) {
            return;
        }
        const RoomKey& room = playerRoom_[originPid];
        if (room.room < 0) {
            return;
        }
        SendToAllInRoom(room, type, payload, originPid);
        return;
    }
    // Star: relay to every joined peer except the origin.
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

void Session::RouteCombatIntent(u8 originPeer, const PayloadUnion& payload) {
    // M4 room ownership (network.md §6): CombatIntent is validated by the
    // room's OWNER. The attacker can only hit enemies in its own room, so the
    // target room is the attacker's current room. Route to the owner peer;
    // when the host is the owner (its own room default) consume locally.
    const u8 attacker = payload.combatIntent.attackerId;
    if (attacker >= kMaxLocalPlayers || !roster_[attacker].present) {
        NetLog.warn("net: CombatIntent from unknown attacker {}; dropping", attacker);
        return;
    }
    const u8 owner = ownership_.OwnerOf(playerRoom_[attacker].stage, playerRoom_[attacker].room);
    if (owner == kInvalidPlayerId) {
        // No owner for the attacker's room (ownerless room / room unknown):
        // consume locally so the host's validation rejects it visibly rather
        // than silently dropping a client's hit.
        NetLog.debug("net: CombatIntent for ownerless room -> host consumes");
        if (gameHandler_) {
            gameHandler_(MsgType::CombatIntent, payload);
        }
        return;
    }
    if (owner == 0) {
        // The host owns the room: consume locally (validation + injection).
        if (gameHandler_) {
            gameHandler_(MsgType::CombatIntent, payload);
        }
        return;
    }
    const u8 ownerPeer = PlayerPeer(owner);
    if (ownerPeer == kInvalidPlayerId) {
        NetLog.warn("net: CombatIntent owner player {} not connected; dropping", owner);
        return;
    }
    NetLog.debug("net: routing CombatIntent from player {} to room owner {} (peer {})",
        attacker, owner, ownerPeer);
    SendToPeer(ownerPeer, MsgType::CombatIntent, payload);
}

void Session::BroadcastOwnership(const std::vector<RoomKey>& changed, u8 exceptPlayer) {
    for (const auto& key : changed) {
        PayloadUnion payload = {};
        std::strncpy(payload.roomOwnership.stage, key.stage, kMaxStageNameLength - 1);
        payload.roomOwnership.stage[kMaxStageNameLength - 1] = '\0';
        payload.roomOwnership.room = key.room;
        payload.roomOwnership.owner = ownership_.OwnerOf(key.stage, key.room);
        SendToAll(MsgType::RoomOwnership, payload, exceptPlayer);
        NetLog.info("net: room {}:{} owner -> {}", key.stage, static_cast<s32>(key.room),
            payload.roomOwnership.owner);
    }
}

void Session::SendOwnershipMap(u8 peerIndex) {
    std::vector<RoomKey> owned;
    ownership_.OwnedRooms(owned);
    for (const auto& key : owned) {
        PayloadUnion payload = {};
        std::strncpy(payload.roomOwnership.stage, key.stage, kMaxStageNameLength - 1);
        payload.roomOwnership.stage[kMaxStageNameLength - 1] = '\0';
        payload.roomOwnership.room = key.room;
        payload.roomOwnership.owner = ownership_.OwnerOf(key.stage, key.room);
        SendToPeer(peerIndex, MsgType::RoomOwnership, payload);
    }
}

void Session::UpdatePlayerRoom(u8 pid, const char* stage, s8 roomNo, bool isHost) {
    if (pid >= kMaxLocalPlayers || roomNo < 0) {
        return;
    }
    // Transient pre-load states (a boot/logo scene, a not-yet-set start stage)
    // carry an empty stage; they must not create a bogus ("", room) ownership
    // entry — the first real PlayerState with a stage establishes the room.
    if (stage == nullptr || stage[0] == '\0') {
        return;
    }
    RoomKey& cur = playerRoom_[pid];
    const bool sameStage = std::strcmp(cur.stage, stage ? stage : "") == 0;
    if (sameStage && cur.room == roomNo) {
        return;  // no move
    }
    std::vector<RoomKey> changed;
    if (cur.room >= 0) {
        ownership_.OnPlayerLeave(cur.stage, cur.room, pid, isHost, &changed);
    }
    std::strncpy(cur.stage, stage ? stage : "", kMaxStageNameLength - 1);
    cur.stage[kMaxStageNameLength - 1] = '\0';
    cur.room = roomNo;
    ownership_.OnPlayerEnter(cur.stage, cur.room, pid, isHost, &changed);
    if (role_ == SessionRole::Host) {
        BroadcastOwnership(changed);
    }
}

void Session::setLocalRoom(const char* stage, s8 roomNo) {
    if (role_ != SessionRole::Host) {
        return;
    }
    // The host's own room drives ownership (host-defaults-own-its-room) and
    // the EnemySnapshot fan-out scope. Sticky: entering a room never steals
    // ownership from an existing owner UNLESS the host is entering — the
    // host-default rule wins by design.
    UpdatePlayerRoom(0, stage, roomNo, /*isHost=*/true);
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

void Session::SendToAllInRoom(const RoomKey& room, MsgType type, const PayloadUnion& payload,
                              u8 exceptPlayer) {
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        if (i == exceptPlayer || !roster_[i].present) {
            continue;
        }
        if (playerRoom_[i].room != room.room ||
            std::strcmp(playerRoom_[i].stage, room.stage) != 0)
        {
            continue;  // M4 same-room scoping: only the room's players get it
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
    // M4: the leaving player's rooms must transfer ownership (sticky rule —
    // next player in the room, else the host) BEFORE the roster slot closes.
    if (role_ == SessionRole::Host) {
        std::vector<RoomKey> changed;
        ownership_.OnPlayerDisconnect(playerId, /*isHost=*/false, &changed);
        BroadcastOwnership(changed);
    }
    playerRoom_[playerId] = RoomKey{};
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
