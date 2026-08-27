#include "dusk/net/session.h"

#include <aurora/lib/logging.hpp>

#include <chrono>
#include <cstring>
#include <string>

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

std::string TrimCopy(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// Stamp the origin PlayerId onto a game message the host is about to
// consume/relay. Clients only see peer 0 (star topology); the payload id
// is whatever the sender wrote. PlayerState/PlayerEvent must not trust
// the payload.
void StampOrigin(Message& msg, PlayerId origin) {
    switch (msg.type) {
    case MsgType::PlayerState:
        msg.payload.playerState.playerId = origin;
        break;
    case MsgType::PlayerEvent:
        msg.payload.playerEvent.playerId = origin;
        break;
    case MsgType::HorseState:
        msg.payload.horseState.playerId = origin;
        break;
    default:
        break;
    }
}

// -------------------------------------------------------------------------
// Star-relay policy: PlayerState/PlayerEvent are star-relayed. v9 dropped
// host->all time/weather (and the unused ghost wire).
// -------------------------------------------------------------------------
enum class RelayPolicy : u8 {
    None, // not star-relayed (handshake)
    Star, // relay to every joined peer except the origin
};

RelayPolicy PolicyFor(MsgType type) {
    switch (type) {
    case MsgType::PlayerState:
    case MsgType::PlayerEvent:
    case MsgType::HorseState:
        return RelayPolicy::Star;
    default:
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
    startFailureReason_ = "";
    endReason_ = SessionEndReason::Shutdown;
    frame_ = 0;
    startedAtMs_ = 0;
    connectedAtMs_ = 0;
    roster_ = {};
    peerToPlayer_.fill(kInvalidPlayerId);
    peerGeneration_.fill(0);
    peerConnectedAtMs_.fill(0);
    worldStage_ = StageInfo{};

    if (!transport_.StartHost(config_.port)) {
        // Capstone MINOR F: surface the distinct cause (port-busy vs
        // already-running vs socket failure) so the glue can log it
        // distinctly instead of a generic "failed to start".
        NetLog.error("net: failed to start host transport on port {}: {}", config_.port,
            transport_.LastStartError());
        startFailureReason_ = transport_.LastStartError();
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
    config_.joinHost = TrimCopy(config_.joinHost);
    if (config_.joinHost.empty()) {
        startFailureReason_ = "join host is empty";
        role_ = SessionRole::None;
        return false;
    }
    if (config_.joinHost.find(':') != std::string::npos) {
        startFailureReason_ = "join host is an IP or hostname (port is net.hostPort)";
        role_ = SessionRole::None;
        return false;
    }
    role_ = SessionRole::Client;
    state_ = SessionState::Idle;
    selfId_ = kInvalidPlayerId;
    rejectReasonName_ = "";
    startFailureReason_ = "";
    endReason_ = SessionEndReason::Shutdown;
    frame_ = 0;
    startedAtMs_ = 0;
    connectedAtMs_ = 0;
    roster_ = {};
    peerToPlayer_.fill(kInvalidPlayerId);
    peerGeneration_.fill(0);
    peerConnectedAtMs_.fill(0);
    worldStage_ = StageInfo{};

    if (!transport_.StartClient(config_.joinHost, config_.port)) {
        // Capstone MINOR F: surface the distinct cause (resolve-failed vs
        // connect-failed vs already-running) for the glue's distinct log.
        NetLog.error("net: failed to start client transport to {}:{}: {}", config_.joinHost,
            config_.port, transport_.LastStartError());
        startFailureReason_ = transport_.LastStartError();
        role_ = SessionRole::None;
        return false;
    }

    state_ = SessionState::Connecting;
    startedAtMs_ = NowMs();
    NetLog.info("net: connecting to {}:{} as '{}'", config_.joinHost, config_.port, config_.name);
    return true;
}

u64 Session::deadlineRemainMs() const {
    u64 start = 0;
    if (state_ == SessionState::Connecting) {
        start = startedAtMs_;
    } else if (state_ == SessionState::Connected) {
        start = connectedAtMs_;
    } else {
        return 0;
    }
    if (start == 0 || config_.joinTimeoutMs == 0) {
        return 0;
    }
    const u64 now = NowMs();
    if (now < start) {
        return 0;
    }
    const u64 elapsed = now - start;
    if (elapsed >= config_.joinTimeoutMs) {
        return 0;
    }
    return config_.joinTimeoutMs - elapsed;
}

void Session::Stop() {
    // Capstone MAJOR 1 (review-full-deepseek-v4-flash-0731.md MAJOR 1): Stop()
    // must tear down the transport whenever it is running, INDEPENDENT of
    // state_. A session that ended from the host's side (HandleDisconnect /
    // OnSessionEnd set Ended with NO transport_.Stop()) previously dead-ended
    // every later StartClient/StartHost in the same process: Stop() returned
    // early for Ended, the transport kept running (socket thread + ENet host),
    // and the next start hit "start client called on an already-running
    // transport" forever until an app restart.
    //
    // The leave/end messages are still enqueued BEFORE the teardown:
    // Transport::Stop() joins the socket thread after a final outbox drain,
    // so the goodbye messages go out.
    if (transport_.IsRunning()) {
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
    }
    if (state_ == SessionState::Idle || state_ == SessionState::Ended) {
        return;  // never started, or already ended — transport (if any) is down now
    }
    state_ = SessionState::Ended;
    NetLog.info("net: session stopped");
}

void Session::Update() {
    if (state_ == SessionState::Idle || state_ == SessionState::Ended ||
        state_ == SessionState::Rejected)
    {
        return;
    }
    ++frame_;

    InboundPacket pkt;
    while (transport_.Poll(pkt)) {
        switch (pkt.type) {
        case NetEventType::Connected:
            HandleConnect(pkt.peerIndex, pkt.generation);
            break;
        case NetEventType::Disconnected:
            HandleDisconnect(pkt.peerIndex);
            break;
        case NetEventType::Data:
            HandleData(pkt);
            break;
        }
    }

    // Client connect deadline: Connecting must become Connected in time.
    // Without this, a dead Join Host IP sits in Connecting until ENet's
    // 5–30 s peer timeout, and the Network tab had nothing to show.
    if (role_ == SessionRole::Client && state_ == SessionState::Connecting && startedAtMs_ != 0 &&
        NowMs() - startedAtMs_ >= config_.joinTimeoutMs)
    {
        startFailureReason_ = "connection timed out";
        NetLog.warn("net: connect timed out after {} ms", config_.joinTimeoutMs);
        endReason_ = SessionEndReason::ConnectionLost;
        if (transport_.IsRunning()) {
            transport_.Stop();
        }
        state_ = SessionState::Ended;
    }

    // Client join deadline: Connected must turn into Joined in time.
    if (role_ == SessionRole::Client && state_ == SessionState::Connected && connectedAtMs_ != 0 &&
        NowMs() - connectedAtMs_ >= config_.joinTimeoutMs)
    {
        rejectReasonName_ = "join timed out";
        NetLog.warn("net: join timed out after {} ms", config_.joinTimeoutMs);
        DisconnectRejected();
    }

    // Host: a connected peer that never sends JoinRequest occupies an ENet
    // slot forever. Bound that wait with the same deadline as client join.
    if (role_ == SessionRole::Host && state_ == SessionState::Listening) {
        for (u8 i = 0; i < Transport::kMaxPeers; ++i) {
            if (peerConnectedAtMs_[i] == 0 || peerToPlayer_[i] != kInvalidPlayerId) {
                continue;
            }
            if (NowMs() - peerConnectedAtMs_[i] >= config_.joinTimeoutMs) {
                NetLog.warn("net: peer {} sent no JoinRequest in {} ms; disconnecting", i,
                    config_.joinTimeoutMs);
                transport_.DisconnectPeer(i);
                peerConnectedAtMs_[i] = 0;
            }
        }
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

void Session::HandleConnect(u8 peerIndex, u16 generation) {
    if (role_ == SessionRole::Host) {
        if (peerIndex < Transport::kMaxPeers) {
            // Missed disconnect + slot reuse: drop the stale roster mapping
            // before this connection can inherit the previous PlayerId.
            const PlayerId stale = peerToPlayer_[peerIndex];
            if (stale != kInvalidPlayerId) {
                NetLog.warn("net: peer {} reused with player {} still mapped; removing", peerIndex,
                    stale);
                RemovePlayer(stale, /*broadcastLeave=*/true);
            }
            peerToPlayer_[peerIndex] = kInvalidPlayerId;
            peerGeneration_[peerIndex] = generation;
            peerConnectedAtMs_[peerIndex] = NowMs();
        }
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
            peerGeneration_[peerIndex] = 0;
            peerConnectedAtMs_[peerIndex] = 0;
        }
        return;
    }
    // Client: the host went away (no SessionEnd arrived — ENet detected the
    // dead peer). Distinct end reason for the host-leave UX (M4 D8).
    // Capstone MAJOR 1: no transport_.Stop() here on purpose for a Joined
    // session — the coop glue calls Stop() the frame it observes Ended.
    // Connecting (ENet handshake never completed) has no JoinReject coming,
    // so tear the transport down as a start-failure. Connected (JoinRequest
    // in flight) must NOT Stop here: Stop() would drain the inbox and drop
    // a JoinReject still queued behind this disconnect.
    if (state_ == SessionState::Rejected || state_ == SessionState::Ended ||
        state_ == SessionState::Idle)
    {
        return;
    }
    NetLog.warn("net: connection to host lost");
    const SessionState previous = state_;
    if (previous == SessionState::Connecting) {
        startFailureReason_ = "could not reach the host";
        endReason_ = SessionEndReason::ConnectionLost;
        state_ = SessionState::Ended;
        if (transport_.IsRunning()) {
            transport_.Stop();
        }
        return;
    }
    if (previous == SessionState::Connected) {
        startFailureReason_ = "join interrupted";
    }
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
    if (pkt.channel != ChannelFor(msg.type)) {
        NetLog.warn("net: dropping {} from peer {} on channel {} (expected {})",
            static_cast<u16>(msg.type), pkt.peerIndex, pkt.channel, ChannelFor(msg.type));
        return;
    }
    switch (msg.type) {
    case MsgType::JoinRequest:
        if (role_ == SessionRole::Host) {
            OnJoinRequest(pkt.peerIndex, msg, pkt.generation);
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
        OnPlayerLeave(pkt.peerIndex, msg);
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
    case MsgType::HorseState:
        // Game traffic: the session owns transport/roster only. The game
        // side consumes the message; on the host the message is then
        // star-relayed. Origin stamp: overwrite the payload player id from
        // the peer map so a client cannot impersonate another slot.
        if (role_ == SessionRole::Host) {
            if (pkt.peerIndex >= Transport::kMaxPeers) {
                break;
            }
            const PlayerId origin = peerToPlayer_[pkt.peerIndex];
            if (origin == kInvalidPlayerId) {
                NetLog.warn("net: dropping game message from unjoined peer {}", pkt.peerIndex);
                break;
            }
            StampOrigin(msg, origin);
        }
        if (gameHandler_) {
            gameHandler_(msg.type, msg.payload);
        }
        if (role_ == SessionRole::Host) {
            ForwardGameMessage(pkt.peerIndex, msg.type, msg.payload);
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

void Session::OnJoinRequest(u8 peerIndex, const Message& msg, u16 generation) {
    if (peerIndex >= Transport::kMaxPeers) {
        return;
    }
    // Same-connection re-JoinRequest must not get a second slot. A different
    // generation on this index is slot reuse after a missed disconnect —
    // drop the stale mapping and continue the join.
    if (peerToPlayer_[peerIndex] != kInvalidPlayerId) {
        if (generation == peerGeneration_[peerIndex]) {
            NetLog.warn("net: duplicate JoinRequest from peer {} (already player {}); ignoring",
                peerIndex, peerToPlayer_[peerIndex]);
            return;
        }
        NetLog.warn("net: JoinRequest on reused peer {} (was player {}); replacing", peerIndex,
            peerToPlayer_[peerIndex]);
        RemovePlayer(peerToPlayer_[peerIndex], /*broadcastLeave=*/true);
    }
    peerGeneration_[peerIndex] = generation;
    const auto& req = msg.payload.joinRequest;

    if (req.version != kProtocolVersion) {
        PayloadUnion payload = {};
        payload.joinReject.reason = static_cast<u8>(JoinRejectReason::VersionMismatch);
        SendToPeer(peerIndex, MsgType::JoinReject, payload);
        transport_.DisconnectPeer(peerIndex);
        peerConnectedAtMs_[peerIndex] = 0;
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
        transport_.DisconnectPeer(peerIndex);
        peerConnectedAtMs_[peerIndex] = 0;
        NetLog.warn("net: rejecting peer {} (slot {} unavailable)", peerIndex, req.requestedSlot);
        return;
    }

    if (PresentCount() >= config_.maxPlayers) {
        PayloadUnion payload = {};
        payload.joinReject.reason = static_cast<u8>(JoinRejectReason::SessionFull);
        SendToPeer(peerIndex, MsgType::JoinReject, payload);
        transport_.DisconnectPeer(peerIndex);
        peerConnectedAtMs_[peerIndex] = 0;
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

    // JoinAccept: assigned id + full roster + world stage (00-network.md §4).
    PayloadUnion accept = {};
    accept.joinAccept.assignedPlayerId = id;
    accept.joinAccept.stage = worldStage_;
    FillWireRoster(accept.joinAccept.roster);
    SendToPeer(peerIndex, MsgType::JoinAccept, accept);

    // WorldInit: fixed stage + roster (00-network.md §4/§5); M1's
    // snapshot-on-join is a burst of ordinary per-frame PlayerState messages
    // sent right after this.
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
    if (state_ != SessionState::Connected) {
        return;  // ignore a late accept after timeout/reject
    }
    const auto& accept = msg.payload.joinAccept;
    // Semantic validation on receive (deepseek M5): never store an out-of-
    // range assigned id, and require the roster to back the assignment (the
    // host always sends both; anything else is a buggy/forged accept).
    if (accept.assignedPlayerId >= kMaxLocalPlayers) {
        NetLog.warn("net: JoinAccept assigns invalid player id {}; rejecting",
            accept.assignedPlayerId);
        rejectReasonName_ = "invalid join accept";
        DisconnectRejected();
        return;
    }
    if (accept.roster[accept.assignedPlayerId].present == 0) {
        NetLog.warn("net: JoinAccept roster does not mark player {} present; rejecting",
            accept.assignedPlayerId);
        rejectReasonName_ = "invalid join accept";
        DisconnectRejected();
        return;
    }
    selfId_ = accept.assignedPlayerId;
    worldStage_ = accept.stage;
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
    DisconnectRejected();
}

void Session::OnPlayerLeave(u8 originPeer, const Message& msg) {
    if (role_ == SessionRole::Host) {
        if (originPeer >= Transport::kMaxPeers) {
            return;
        }
        // Bind leave to the sending peer — never trust the payload id (a
        // client must not be able to kick another slot, including the host).
        const PlayerId pid = peerToPlayer_[originPeer];
        if (pid == kInvalidPlayerId || pid == 0) {
            return;
        }
        RemovePlayer(pid, /*broadcastLeave=*/true);
        return;
    }
    const u8 playerId = msg.payload.playerLeave.playerId;
    if (playerId >= kMaxLocalPlayers) {
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
    // Capstone MAJOR 1: the host-side end must remain tearable by a later
    // Stop() call (which now always stops the transport when running) so a
    // new session can start in the same process without an app restart.
}

void Session::OnWorldInit(const Message& msg) {
    const auto& init = msg.payload.worldInit;
    // Capstone MINOR 8 (review-full-glm-5.2.md MINOR 8): on a client,
    // worldStage_ is ONLY a join-time reference ("where is the host" under
    // the stay-put join policy) — no client consumer reads worldStage() (the
    // puppet gate keys on the remote's REAL stage from PlayerState). Every
    // later roster-refresh WorldInit overwrites it with the host's CURRENT
    // stage; harmless today, but a future "where is the host" UI marker
    // would snap. The write stays unconditional by design; scope it to the
    // joining peer if a consumer is added.
    worldStage_ = init.stage;
    ApplyRoster(init.roster);  // roster refresh; may include players who joined later
    u8 present = 0;
    for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
        if (init.roster[i].present != 0) {
            ++present;
        }
    }
    NetLog.info("net: world init: stage '{}' room {} (players {})", init.stage.stage,
        static_cast<s32>(init.stage.room), present);
}

// ---------------------------------------------------------------------------
// Game-message routing (star topology)
// ---------------------------------------------------------------------------

bool Session::SendGameMessage(MsgType type, const PayloadUnion& payload) {
    if (state_ != SessionState::Listening && state_ != SessionState::Joined) {
        return false;
    }
    if (role_ == SessionRole::Host) {
        // Uniform host->all fan-out (no room-scoped branches).
        SendToAll(type, payload);
    } else {
        // Client: everything flows to the host, which relays to the others.
        SendToPeer(0, type, payload);
    }
    return true;
}

void Session::ForwardGameMessage(u8 originPeer, MsgType type, const PayloadUnion& payload) {
    // Star-relay policy seam (MINOR d): see PolicyFor. v7: Star only — relay
    // to every joined peer except the origin.
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
    for (u8 i = 0; i < Transport::kMaxPeers; ++i) {
        if (peerToPlayer_[i] == playerId) {
            peerToPlayer_[i] = kInvalidPlayerId;
        }
    }
    NetLog.info("net: player {} left the session", playerId);
    if (broadcastLeave) {
        PayloadUnion payload = {};
        payload.playerLeave.playerId = playerId;
        SendToAll(MsgType::PlayerLeave, payload, /*exceptPlayer=*/playerId);
    }
}

void Session::DisconnectRejected() {
    state_ = SessionState::Rejected;
    if (transport_.IsRunning()) {
        transport_.Stop();
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
