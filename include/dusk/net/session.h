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
    std::string joinHost = "127.0.0.1";   // client: host IP or hostname (no :port)
    u32 version = kProtocolVersion;       // carried in JoinRequest
    u8 requestedSlot = kAnySlot;          // client: preferred PlayerId
    u8 maxPlayers = kMaxLocalPlayers;     // host: roster cap (selftest uses 2)
    u64 joinTimeoutMs = 8000;             // Connecting, Connected-join, and host unjoined-peer deadline
    /// Host's world info sent in JoinAccept/WorldInit. M0 seeds it statically;
    /// M1+ fills it from the real sim (stage/room/spawn).
    StageInfo stage;
};

/// Game-side consumer of messages the session does not own (PlayerState,
/// PlayerEvent). Invoked on the game thread inside Update()/HandleData with
/// the already-parsed payload. On the host the handler runs for every inbound
/// game message and the message is then relayed to every other joined peer
/// (star topology, 00-network.md §2); on a client the handler runs for
/// messages received from the host. Every game message is star-relayed
/// (PlayerState/PlayerEvent); there is no room-scoped relay.
using GameMessageHandler = std::function<void(MsgType type, const PayloadUnion& payload)>;

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
    /// messages to the other peers automatically (HandleData). Host-side
    /// fan-out is uniform — every game message reaches every joined peer
    /// (no room-scoped branches).
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
    /// Stage from the last JoinAccept/WorldInit (client) — where the host
    /// was at handshake time; on the host this is the value published with
    /// setWorldStage (filled every frame from the real sim) and is what
    /// joiners receive.
    [[nodiscard]] const StageInfo& worldStage() const { return worldStage_; }

    /// Host-side: publish the current world stage so a mid-game joiner is
    /// told where the host's world is (JoinAccept/WorldInit carry; M4.5:
    /// join-warp is REMOVED — stay-put join — so the carry is the joiner's
    /// "where is the host" reference and feeds the cross-stage `stageOk`
    /// puppet gate; it no longer drives a warp decision). The coop tick
    /// calls this every frame from the real Link; the session snapshots the
    /// latest value into the join handshake.
    void setWorldStage(const StageInfo& s) { worldStage_ = s; }
    /// Client-side: why the session ended (for the host-leave UX / toasts).
    [[nodiscard]] SessionEndReason endReason() const { return endReason_; }

    [[nodiscard]] u64 sessionFrames() const { return frame_; }
    [[nodiscard]] const char* rejectReasonName() const { return rejectReasonName_; }
    /// Capstone MINOR F: why the most recent StartHost/StartClient failed
    /// ("" when none), for the glue's distinct failure logging.
    [[nodiscard]] const char* startFailureReason() const { return startFailureReason_; }
    /// Remaining Connecting / Connected-join deadline, or 0 if none.
    [[nodiscard]] u64 deadlineRemainMs() const;
    [[nodiscard]] const std::string& configJoinHost() const { return config_.joinHost; }
    [[nodiscard]] u16 configPort() const { return config_.port; }
    [[nodiscard]] const std::string& configName() const { return config_.name; }

private:
    // -- transport event handlers (game thread) --
    void HandleConnect(u8 peerIndex, u16 generation);
    void HandleDisconnect(u8 peerIndex);
    void HandleData(const InboundPacket& pkt);

    // -- message dispatch --
    void OnJoinRequest(u8 peerIndex, const Message& msg, u16 generation);
    void OnJoinAccept(const Message& msg);
    void OnJoinReject(const Message& msg);
    void OnPlayerLeave(u8 originPeer, const Message& msg);
    void OnSessionEnd(const Message& msg);
    void OnWorldInit(const Message& msg);
    /// Client: mark Rejected and tear the transport down (no PlayerLeave —
    /// we never joined). Frees the host's ENet peer slot / roster entry via
    /// the resulting disconnect. State stays Rejected so the glue can toast.
    void DisconnectRejected();

    // -- game-message routing (host relay, star topology) --
    void ForwardGameMessage(u8 originPeer, MsgType type, const PayloadUnion& payload);

    // -- send helpers --
    void SendToPeer(u8 peerIndex, MsgType type, const PayloadUnion& payload);
    void SendToAll(MsgType type, const PayloadUnion& payload, u8 exceptPlayer = kInvalidPlayerId);

    // -- host logic --
    PlayerId AssignPlayerId(u8 requestedSlot);
    u8 PresentCount() const;
    u8 PlayerPeer(u8 playerId) const;
    void RemovePlayer(u8 playerId, bool broadcastLeave);
    void ApplyRoster(const std::array<PlayerInfo, kMaxLocalPlayers>& wireRoster);
    void FillWireRoster(std::array<PlayerInfo, kMaxLocalPlayers>& out) const;

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
    std::array<u16, Transport::kMaxPeers> peerGeneration_{};
    std::array<u64, Transport::kMaxPeers> peerConnectedAtMs_{};
    GameMessageHandler gameHandler_;
    StageInfo worldStage_;
    u64 frame_ = 0;
    u64 startedAtMs_ = 0;     // client: StartClient → Connecting deadline
    u64 connectedAtMs_ = 0;   // client: ENet connected → JoinAccept deadline
    // Last-observed reliable-ring drop counters (for surfacing explicit
    // reliable-overflow failures in Update()).
    u64 lastReliableOutboundDropped_ = 0;
    u64 lastReliableInboundDropped_ = 0;
};

}  // namespace dusk::net
