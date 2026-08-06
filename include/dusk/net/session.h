#pragma once

/**
 * \file session.h
 * Session lifecycle (docs/design/mod-coop/00-network.md §4).
 *
 * Host:   Idle -> Listening (accepting joins; roster grows as peers join)
 * Client: Idle -> Connecting -> Connected -> Joined | Rejected
 * Either side: -> Ended (SessionEnd received/sent, host gone, or Stop()).
 *
 * The host assigns session-wide PlayerIds (0..kMaxLocalPlayers-1), tracks the
 * roster, and validates joins (version, slot, capacity). All session logic
 * runs on the game thread via Update(); the only thread in the module is the
 * transport's socket thread, whose events arrive as inbound packets.
 */

#include "dusk/net/protocol.h"
#include "dusk/net/transport.h"

#include <array>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dusk::net {

enum class SessionRole : u8 {
    None = 0,
    Host,
    Client,
};

enum class SessionState : u8 {
    Idle = 0,
    Listening,  // host: transport up, accepting joins
    Connecting, // client: ENet connect in flight
    Connected,  // client: ENet connected, JoinRequest sent
    Joined,     // both: accepted into the session
    Rejected,   // client: JoinReject received or join deadline expired
    Ended,      // session terminated
};

/// One player tracked by the session (runtime view; converted to the wire
/// PlayerInfo when building JoinAccept/WorldInit).
struct PlayerSlot {
    PlayerId playerId = kInvalidPlayerId;
    bool present = false;
    char name[kMaxNameLength] = {};
};

struct SessionConfig {
    u16 port = 44770;  // host listen port; 0 = ephemeral (BoundPort())
    std::string name = "Dusklight co-op";  // host: session name; client: player name
    std::string joinHost = "127.0.0.1";   // client: host IP
    u32 version = kProtocolVersion;       // carried in JoinRequest
    u8 requestedSlot = kAnySlot;          // client: preferred PlayerId
    u8 maxPlayers = kMaxLocalPlayers;     // host: roster cap (selftest uses 2)
    u64 joinTimeoutMs = 8000;             // client: Connected -> Joined deadline
    /// Host's world info sent in JoinAccept/WorldInit. M0 seeds it statically;
    /// M1+ fills it from the real sim (stage/room/spawn).
    StageInfo stage;
};

/// Game-side consumer of messages the session does not own (PlayerState,
/// PlayerEvent, and later EnemySnapshot/combat/time). Invoked on the game
/// thread inside Update()/HandleData with the already-parsed payload. On the
/// host the handler runs for every inbound game message and the message is
/// then relayed to every other joined peer (star topology, 00-network.md §2);
/// on a client the handler runs for messages received from the host.
using GameMessageHandler = std::function<void(MsgType type, const PayloadUnion& payload)>;

// ---------------------------------------------------------------------------
// Room ownership (docs/design/network.md §6; M4) — the distributed authority
// table the host maintains and broadcasts via RoomOwnershipMsg.
//
// Rules: owner = the world host when the host is present in the room,
// otherwise the first player to arrive. Ownership is STICKY — it transfers
// only on leave/disconnect, never on arrival (no ping-pong); when the owner
// leaves, ownership passes to the next player still in the room (lowest
// arrival order after the leaver), or the room becomes ownerless. A player
// entering an ownerless room becomes its owner (first-in).
// ---------------------------------------------------------------------------

struct RoomKey {
    char stage[kMaxStageNameLength] = {};
    s8 room = -1;
    bool operator==(const RoomKey& o) const {
        return room == o.room && std::strcmp(stage, o.stage) == 0;
    }
};

class RoomOwnershipTable {
public:
    RoomOwnershipTable() = default;

    /// Player `pid` entered (stage, room). `isHost` lets the host-default rule
    /// win over arrival order. Appends any room whose owner CHANGED to
    /// `changed` (a takeover counts). Returns the owner after the update.
    u8 OnPlayerEnter(const char* stage, s8 room, u8 pid, bool isHost,
                     std::vector<RoomKey>* changed);
    /// Player `pid` left (stage, room) — room change or leave. Appends the
    /// room to `changed` when the owner changed (transfer / ownerless).
    void OnPlayerLeave(const char* stage, s8 room, u8 pid, bool isHost,
                       std::vector<RoomKey>* changed);
    /// Player disconnected: leaves every room it was present in.
    void OnPlayerDisconnect(u8 pid, bool isHost, std::vector<RoomKey>* changed);
    /// Current owner of (stage, room), or kInvalidPlayerId when ownerless.
    [[nodiscard]] u8 OwnerOf(const char* stage, s8 room) const;
    /// Every currently-owned room, for the join-time map broadcast.
    void OwnedRooms(std::vector<RoomKey>& out) const;
    /// Client-side: apply an authoritative owner assignment from the host
    /// (RoomOwnershipMsg). Replaces the stored owner for (stage, room);
    /// kInvalidPlayerId clears the entry (ownerless).
    void SetOwner(const char* stage, s8 room, u8 owner);
    /// Host-side: drop entries the host cannot legitimately own (ownerless
    /// entries, and host-owned entries the host is not actually in). Called
    /// from setLocalRoom to sweep stale entries left by a stage-transition
    /// race (capstone MINOR 5).
    void SweepStaleHostEntries();
    void Clear() { rooms_.clear(); }

private:
    struct RoomState {
        RoomKey key;
        u8 owner = kInvalidPlayerId;
        std::array<u8, kMaxLocalPlayers> order{};  // arrival order
        u8 orderLen = 0;
    };
    RoomState* Find(const RoomKey& key);
    const RoomState* Find(const RoomKey& key) const;
    void RecomputeOwner(RoomState& rs, bool isHost, std::vector<RoomKey>* changed);

    std::vector<RoomState> rooms_;  // small (one per occupied room); linear is fine
};

class Session {
public:
    Session() = default;
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// Start a host session (Listening) or a client session (Connecting).
    /// Both are async: drive them from the game thread with Update().
    bool StartHost(const SessionConfig& config);
    bool StartClient(const SessionConfig& config);

    /// Graceful teardown: host broadcasts SessionEnd, a joined client sends
    /// PlayerLeave; then the transport is stopped and its thread joined.
    void Stop();

    /// Drain the transport inbox and advance the state machine. Call once per
    /// game frame while the session is active.
    void Update();

    /// Registers the game-side message consumer (M1: puppet apply on the
    /// receiver; the sender path uses SendGameMessage). Only one handler is
    /// held; pass {} to clear.
    void SetGameMessageHandler(GameMessageHandler handler) { gameHandler_ = std::move(handler); }

    /// Sends a game-side message (PlayerState/PlayerEvent/...) into the
    /// session: on a client to the host (peer 0), on the host to every joined
    /// peer (the host never receives its own sends back). Returns false when
    /// the session is not in a playable state. The host relays inbound game
    /// messages to the other peers automatically (HandleData). M4: the
    /// host-side fan-out is per-type — PlayerState/PlayerEvent to every peer,
    /// EnemySnapshot only to peers in the sender's room, owner events
    /// (CombatResult/EnemyEvent) to every peer.
    bool SendGameMessage(MsgType type, const PayloadUnion& payload);

    [[nodiscard]] SessionRole role() const { return role_; }
    [[nodiscard]] SessionState state() const { return state_; }
    [[nodiscard]] PlayerId selfId() const { return selfId_; }
    [[nodiscard]] u16 boundPort() const { return transport_.BoundPort(); }
    /// Capstone MAJOR 1: whether the ENet transport is currently running (its
    /// socket thread is alive). Exposed so the coop glue / selftest can
    /// observe that Stop() tears the transport down even for an Ended session.
    [[nodiscard]] bool transportRunning() const { return transport_.IsRunning(); }
    [[nodiscard]] const std::array<PlayerSlot, kMaxLocalPlayers>& roster() const { return roster_; }
    /// Stage/time/weather from the last JoinAccept/WorldInit (client) — what
    /// the host believes the world looks like; on the host these are the
    /// values the game side published with setWorldTime/Weather/Stage (M3/M4:
    /// filled every frame from the real sim) and are what joiners receive.
    [[nodiscard]] const StageInfo& worldStage() const { return worldStage_; }
    [[nodiscard]] const TimeStateInfo& worldTime() const { return worldTime_; }
    [[nodiscard]] const WeatherStateInfo& worldWeather() const { return worldWeather_; }

    /// Host-side: publish the current world stage so a mid-game joiner is
    /// told where the host's world is (JoinAccept/WorldInit carry; M4.5:
    /// join-warp is REMOVED — stay-put join — so the carry is the joiner's
    /// "where is the host" reference and feeds the cross-stage `stageOk`
    /// puppet gate; it no longer drives a warp decision). The coop tick
    /// calls these every frame from the real Link; the session snapshots the
    /// latest value into the join handshake.
    void setWorldStage(const StageInfo& s) { worldStage_ = s; }
    void setWorldTime(const TimeStateInfo& t) { worldTime_ = t; }
    void setWorldWeather(const WeatherStateInfo& w) { worldWeather_ = w; }
    /// Host-side: publish the local player's current (stage, room). The host
    /// uses this to key the room-ownership table (its own room defaults to
    /// host-owned) and to room-scope its EnemySnapshot fan-out. Clients'
    /// rooms come from the PlayerState/PlayerEvent stream.
    void setLocalRoom(const char* stage, s8 roomNo);

    /// M4 room ownership (network.md §6): the owner of (stage, room), or
    /// kInvalidPlayerId. On the host this is the authoritative table; on a
    /// client it is the last RoomOwnershipMsg view.
    [[nodiscard]] u8 roomOwner(const char* stage, s8 room) const {
        return ownership_.OwnerOf(stage, room);
    }
    /// M4: the session's last-known room for a player (host: from the
    /// PlayerState/PlayerEvent stream + setLocalRoom; client: empty).
    [[nodiscard]] const RoomKey& playerRoom(u8 pid) const {
        return pid < kMaxLocalPlayers ? playerRoom_[pid] : emptyRoom_;
    }
    /// Client-side: why the session ended (for the host-leave UX / toasts).
    [[nodiscard]] SessionEndReason endReason() const { return endReason_; }

    [[nodiscard]] u64 sessionFrames() const { return frame_; }
    [[nodiscard]] const char* rejectReasonName() const { return rejectReasonName_; }
    /// Capstone MINOR F: why the most recent StartHost/StartClient failed
    /// ("" when none), for the glue's distinct failure logging.
    [[nodiscard]] const char* startFailureReason() const { return startFailureReason_; }

private:
    // -- transport event handlers (game thread) --
    void HandleConnect(u8 peerIndex);
    void HandleDisconnect(u8 peerIndex);
    void HandleData(const InboundPacket& pkt);

    // -- message dispatch --
    void OnJoinRequest(u8 peerIndex, const Message& msg);
    void OnJoinAccept(const Message& msg);
    void OnJoinReject(const Message& msg);
    void OnPlayerLeave(const Message& msg);
    void OnSessionEnd(const Message& msg);
    void OnWorldInit(const Message& msg);
    void OnRoomOwnership(const Message& msg);

    // -- game-message routing (host relay, star topology; M4 room routing) --
    void ForwardGameMessage(u8 originPeer, MsgType type, const PayloadUnion& payload);
    void RouteCombatIntent(u8 originPeer, const PayloadUnion& payload);
    void BroadcastOwnership(const std::vector<RoomKey>& changed, u8 exceptPlayer = kInvalidPlayerId);
    void SendOwnershipMap(u8 peerIndex);

    // -- send helpers --
    void SendToPeer(u8 peerIndex, MsgType type, const PayloadUnion& payload);
    void SendToAll(MsgType type, const PayloadUnion& payload, u8 exceptPlayer = kInvalidPlayerId);
    void SendToAllInRoom(const RoomKey& room, MsgType type, const PayloadUnion& payload,
                         u8 exceptPlayer);

    // -- host logic --
    PlayerId AssignPlayerId(u8 requestedSlot);
    u8 PresentCount() const;
    u8 PlayerPeer(u8 playerId) const;
    void RemovePlayer(u8 playerId, bool broadcastLeave);
    void ApplyRoster(const std::array<PlayerInfo, kMaxLocalPlayers>& wireRoster);
    void FillWireRoster(std::array<PlayerInfo, kMaxLocalPlayers>& out) const;
    void UpdatePlayerRoom(u8 pid, const char* stage, s8 roomNo, bool isHost);

    Transport transport_;
    SessionConfig config_;
    SessionRole role_ = SessionRole::None;
    SessionState state_ = SessionState::Idle;
    PlayerId selfId_ = kInvalidPlayerId;
    const char* rejectReasonName_ = "";
    const char* startFailureReason_ = "";
    SessionEndReason endReason_ = SessionEndReason::Shutdown;
    std::array<PlayerSlot, kMaxLocalPlayers> roster_{};
    std::array<PlayerId, Transport::kMaxPeers> peerToPlayer_{};
    GameMessageHandler gameHandler_;
    StageInfo worldStage_;
    TimeStateInfo worldTime_;
    WeatherStateInfo worldWeather_;
    // M4: per-player last-known (stage, room) — the host sniffs it from the
    // PlayerState/PlayerEvent stream and from setLocalRoom (the host itself).
    std::array<RoomKey, kMaxLocalPlayers> playerRoom_{};
    RoomOwnershipTable ownership_;
    RoomKey emptyRoom_;
    u64 frame_ = 0;
    u64 connectedAtMs_ = 0;
    // Last-observed reliable-ring drop counters (for surfacing explicit
    // reliable-overflow failures in Update()).
    u64 lastReliableOutboundDropped_ = 0;
    u64 lastReliableInboundDropped_ = 0;
};

}  // namespace dusk::net
