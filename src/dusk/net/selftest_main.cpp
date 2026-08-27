/**
 * \file selftest_main.cpp
 * M0.5 LAN handshake demo + protocol/cadence/robustness self-test.
 *
 * Run (build dir):  ./dusk_net_selftest
 * Exits 0 on success, 1 on any failure.
 *
 * Covers, in one process over loopback (127.0.0.1):
 *   - wire round-trip (serialize -> deserialize -> re-serialize) for all 9
 *     message types (v11), plus malformed-input rejection;
 *   - wire determinism: identical logical messages serialize to identical
 *     bytes (zero-init payload unions — deepseek M3);
 *   - semantic validation: jointCount > kMaxJoints rejected at parse
 *     (deepseek M5), non-finite PlayerState/HorseState poses rejected,
 *     forged JoinAccept (bad assignedPlayerId / roster)
 *     rejected by the client session;
 *   - ring policies: reliable ring overflow is an explicit failure (Send
 *     returns false <=> counter bumps, no silent drops), snapshot rings
 *     replace-newest under pressure (glm MAJOR-1/2); when full they replace
 *     the newest matching PlayerState/HorseState playerId rather than always
 *     the newest slot (multiplexed poses); snapshot *outbox* is
 *     per-peer so host fan-out no longer shares one 128-slot ring;
 *     connect/disconnect use a dedicated lifecycle inbox;
 *   - peer-slot generation guard: connect -> disconnect -> reconnect never
 *     delivers a stale packet from the old connection (deepseek M4);
 *   - duplicate JoinRequest guard: no second slot assigned (deepseek m2);
 *   - oversized inbound packets are dropped and counted (GLM MINOR-5);
 *   - NetClock 60 Hz cadence (exact interval spacing, catch-up);
 *   - host/client session lifecycle: JoinRequest -> JoinAccept + WorldInit,
 *     JoinReject (version mismatch, session full), PlayerLeave relay, and
 *     host SessionEnd;
 *   - M4.6 (capstone): session restart after a remote-side end — a client
 *     whose session Ended from the host's side (graceful SessionEnd, then a
 *     hard connection loss via peer timeout -> HandleDisconnect) gets its
 *     transport torn down by Stop() and starts a second/third session in the
 *     same process without an app restart (review-full-deepseek MAJOR 1);
 *   - session start guards (empty join host, host:port refused);
 *   - Connecting deadline: a client that never reaches ENet Connected ends
 *     with a start-failure reason instead of sitting in Connecting;
 *   - v9/v10: TimeSync/TimeEvent/WeatherChange, GhostSnapshot, EnemyEvent, and
 *     JoinAccept/WorldInit clock/sky fields are gone (out-of-range wire ids
 *     9–13 and 16 rejected); v10 adds a stage name on PlayerEvent; an idle
 *     session refuses game sends;
 *   - clean shutdown: every transport thread joined, every ENet host
 *     destroyed (leak-free exit).
 *
 * The net module logs through aurora::Module; this standalone binary provides
 * the aurora logging globals (mirroring extern/aurora/lib/logging.cpp) so it
 * links without pulling the whole aurora runtime. ENet init/deinit go through
 * the same dusk::net::initialize()/shutdown() the game uses.
 */

#include "dusk/net/clock.h"
#include "dusk/net/module.h"
#include "dusk/net/protocol.h"
#include "dusk/net/session.h"

#include <aurora/aurora.h>
#include <aurora/lib/logging.hpp>
#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Aurora logging shim (standalone binary; the game links the real aurora)
// ---------------------------------------------------------------------------

namespace aurora {
AuroraConfig g_config = {};

void log_internal(AuroraLogLevel level, const char* module, const char* message,
    const unsigned int len) noexcept
{
    (void)level;
    std::fprintf(stderr, "[%s] %.*s\n", module != nullptr ? module : "",
        static_cast<int>(len), message != nullptr ? message : "");
}
}  // namespace aurora

namespace {

using namespace dusk::net;

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (ok) {
        std::printf("  ok: %s\n", what);
    } else {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

u64 NowMs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

// ---------------------------------------------------------------------------
// Pumping helpers (raw transports and sessions)
// ---------------------------------------------------------------------------

/// Polls a raw transport until a packet satisfying `pred` is observed or the
/// deadline expires. Drains everything in between; returns true and fills
/// `matched` on success.
bool WaitPoll(Transport& t, const std::function<bool(const InboundPacket&)>& pred, u64 timeoutMs,
              InboundPacket& matched) {
    const u64 deadline = NowMs() + timeoutMs;
    while (NowMs() < deadline) {
        InboundPacket p;
        while (t.Poll(p)) {
            if (pred(p)) {
                matched = p;
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// Pumps a session until it reaches `want` or the deadline expires.
bool WaitSessionState(Session& s, SessionState want, u64 timeoutMs) {
    const u64 deadline = NowMs() + timeoutMs;
    while (NowMs() < deadline) {
        s.Update();
        if (s.state() == want) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// ---------------------------------------------------------------------------
// Protocol round-trip
// ---------------------------------------------------------------------------

const char* StageName() {
    return "F_SP108";
}

Message MakeMessage(MsgType type) {
    Message m = {};  // zero-init: deterministic wire bytes (deepseek M3)
    m.type = type;
    switch (type) {
    case MsgType::JoinRequest: {
        auto& p = m.payload.joinRequest;
        p.version = kProtocolVersion;
        p.requestedSlot = 3;
        std::strncpy(p.name, "Player Alpha", sizeof(p.name) - 1);
        break;
    }
    case MsgType::JoinAccept: {
        auto& p = m.payload.joinAccept;
        p.assignedPlayerId = 5;
        std::strncpy(p.stage.stage, StageName(), sizeof(p.stage.stage) - 1);
        p.stage.room = 2;
        p.stage.layer = 1;
        p.stage.point = 7;
        for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
            auto& e = p.roster[i];
            e.playerId = i;
            e.present = (i % 2 == 0) ? 1 : 0;
            e.state = static_cast<u8>(PlayerStateId::Connected);
            std::snprintf(e.name, sizeof(e.name), "Roster %u", i);
        }
        break;
    }
    case MsgType::JoinReject: {
        m.payload.joinReject.reason = static_cast<u8>(JoinRejectReason::SessionFull);
        break;
    }
    case MsgType::PlayerLeave: {
        m.payload.playerLeave.playerId = 4;
        break;
    }
    case MsgType::SessionEnd: {
        m.payload.sessionEnd.reason = static_cast<u8>(SessionEndReason::HostLeft);
        break;
    }
    case MsgType::WorldInit: {
        auto& p = m.payload.worldInit;
        std::strncpy(p.stage.stage, StageName(), sizeof(p.stage.stage) - 1);
        p.stage.room = 3;
        p.stage.layer = 0;
        p.stage.point = 11;
        for (u8 i = 0; i < kMaxLocalPlayers; ++i) {
            auto& e = p.roster[i];
            e.playerId = i;
            e.present = (i < 3) ? 1 : 0;
            std::strncpy(e.name, "WorldInit", sizeof(e.name) - 1);
        }
        break;
    }
    case MsgType::PlayerState: {
        auto& p = m.payload.playerState;
        p.playerId = 2;
        p.roomNo = 9;
        p.form = 1;
        p.stateFlags = kPlayerStateFlagRiding | kPlayerStateFlagDemo;
        p.jointCount = 40;
        for (u8 i = 0; i < sizeof(p.scaleFlags); ++i) {
            p.scaleFlags[i] = static_cast<u8>(0xA0 + i);
        }
        p.yaw = 12345;
        p.pitch = -678;
        p.faceBckIdx = 0x1234;
        p.faceBtpIdx = 0x5678;
        p.faceFrame = 42;
        p.reserved = 0xEE;
        p.pos = Vec3f{1.5f, -2.25f, 300.125f};
        for (u8 r = 0; r < 3; ++r) {
            for (u8 c = 0; c < 4; ++c) {
                p.baseTR[r][c] = static_cast<f32>(r * 10 + c) + 0.5f;
                for (u8 j = 0; j < kMaxJoints; ++j) {
                    p.joints[j][r][c] = static_cast<f32>(j * 100 + r * 10 + c) + 0.25f;
                }
            }
        }
        break;
    }
    case MsgType::PlayerEvent: {
        auto& p = m.payload.playerEvent;
        p.playerId = 1;
        p.eventId = static_cast<u8>(PlayerEventId::FormChange);
        p.scene = 4;
        p.data = 0xF0F0F0F0;
        p.data2 = 0x0F0F0F0F;
        std::strncpy(p.stage, "F_SP108", kMaxStageNameLength - 1);
        p.stage[kMaxStageNameLength - 1] = '\0';
        break;
    }
    case MsgType::HorseState: {
        auto& p = m.payload.horseState;
        p.playerId = 3;
        p.roomNo = 4;
        std::strncpy(p.stage, StageName(), sizeof(p.stage) - 1);
        p.jointCount = 38;
        for (u8 i = 0; i < sizeof(p.scaleFlags); ++i) {
            p.scaleFlags[i] = static_cast<u8>(0x50 + i);
        }
        p.yaw = -1111;
        p.reserved = 0xAB;
        p.pos = Vec3f{9.0f, 8.0f, 7.0f};
        for (u8 r = 0; r < 3; ++r) {
            for (u8 c = 0; c < 4; ++c) {
                p.baseTR[r][c] = static_cast<f32>(r * 3 + c) + 0.125f;
                for (u8 j = 0; j < kMaxJoints; ++j) {
                    p.joints[j][r][c] = static_cast<f32>(j * 10 + r + c) + 0.5f;
                }
            }
        }
        break;
    }
    }
    return m;
}

/// Serialize -> deserialize -> re-serialize; the two byte streams must be
/// identical (this also proves WireSize consistency and that the wire layout
/// is stable, since the second serialization goes through the parse).
bool RoundTrip(MsgType type) {
    const Message msg = MakeMessage(type);

    u8 buf1[kMaxMessageSize];
    ByteWriter w1(buf1, sizeof(buf1));
    if (!SerializeMessage(msg, w1)) {
        return false;
    }
    if (w1.size() != 4 + WireSize(type)) {
        return false;
    }

    ByteReader r1(buf1, w1.size());
    Message parsed;
    if (!DeserializeMessage(r1, parsed)) {
        return false;
    }
    if (parsed.type != type) {
        return false;
    }

    u8 buf2[kMaxMessageSize];
    ByteWriter w2(buf2, sizeof(buf2));
    if (!SerializeMessage(parsed, w2)) {
        return false;
    }
    return w1.size() == w2.size() && std::memcmp(buf1, buf2, w1.size()) == 0;
}

void RunProtocolChecks() {
    std::printf("protocol: round-trip all 9 v11 message types\n");
    for (u16 t = static_cast<u16>(MsgType::JoinRequest); t <= static_cast<u16>(MsgType::HorseState);
         ++t)
    {
        const auto type = static_cast<MsgType>(t);
        Check(RoundTrip(type), WireSize(type) > 0 ? "round-trip ok" : "round-trip ok (size 0)");
    }
    Check(WireSize(MsgType::JoinAccept) == 1 + 3 + 20 + kMaxLocalPlayers * 36,
        "JoinAccept wire size");
    Check(WireSize(MsgType::WorldInit) == 20 + kMaxLocalPlayers * 36,
        "WorldInit wire size (stage + roster)");
    Check(WireSize(MsgType::PlayerState) == PlayerStateWireSize(),
        "PlayerState wire size");
    Check(WireSize(MsgType::PlayerEvent) == 4 + 4 + 4 + kMaxStageNameLength,
        "PlayerEvent wire size (v10 stage field)");
    Check(WireSize(MsgType::HorseState) == HorseStateWireSize(),
        "HorseState wire size");
    Check(ChannelFor(MsgType::PlayerState) == kChannelUnreliable &&
              ChannelFor(MsgType::HorseState) == kChannelUnreliable &&
              ChannelFor(MsgType::JoinRequest) == kChannelReliable &&
              ChannelFor(MsgType::PlayerEvent) == kChannelReliable,
        "channel mapping (snapshots unreliable, control reliable)");

    // v9 shred regression: removed combat/ownership/time-weather/ghost wire
    // ids are dead. Type 16 (RoomOwnership) and type 11 (old CombatIntent, then
    // TimeSync) are outside 1..9. Type 9 is HorseState (v11); the old
    // GhostSnapshot layout (id 9, size 34) is rejected by exact-size.
    {
        const u8 removedType[] = {0x10, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};  // type 16
        ByteReader r(removedType, sizeof(removedType));
        Message out;
        Check(!DeserializeMessage(r, out), "removed RoomOwnership wire id (16) rejected");
    }
    {
        const u8 oldIntent[] = {0x0B, 0x00, 0x2A, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // id 11, size 42
        ByteReader r(oldIntent, sizeof(oldIntent));
        Message out;
        Check(!DeserializeMessage(r, out),
            "old CombatIntent/TimeSync wire id 11 rejected (out of range)");
    }
    {
        const u8 oldGhost[] = {0x09, 0x00, 0x22, 0x00, 0, 0, 0, 0};  // id 9, size 34
        ByteReader r(oldGhost, sizeof(oldGhost));
        Message out;
        Check(!DeserializeMessage(r, out),
            "old GhostSnapshot payload size rejected (id 9 is HorseState)");
    }

    // Malformed input must be rejected.
    {
        const u8 truncated[] = {0x01, 0x00};  // type only, no size, no payload
        ByteReader r(truncated, sizeof(truncated));
        Message out;
        Check(!DeserializeMessage(r, out), "truncated header rejected");
    }
    {
        const u8 badSize[] = {0x01, 0x00, 0x00, 0x10, 0x00};  // claims 4096-byte payload
        ByteReader r(badSize, sizeof(badSize));
        Message out;
        Check(!DeserializeMessage(r, out), "lying size field rejected");
    }
    {
        const u8 badType[] = {0x63, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};  // type 99
        ByteReader r(badType, sizeof(badType));
        Message out;
        Check(!DeserializeMessage(r, out), "unknown message type rejected");
    }
    {
        Message ev = MakeMessage(MsgType::PlayerEvent);
        u8 buf[kMaxMessageSize];
        ByteWriter w(buf, sizeof(buf));
        Check(SerializeMessage(ev, w), "PlayerEvent serializes for trailing-junk check");
        buf[w.size()] = 0xFF;
        ByteReader r(buf, static_cast<u16>(w.size() + 1));
        Message out;
        Check(!DeserializeMessage(r, out), "trailing junk after a valid payload is rejected");
    }
    {
        Message ev = MakeMessage(MsgType::PlayerEvent);
        u8 buf[kMaxMessageSize];
        ByteWriter w(buf, sizeof(buf));
        Check(SerializeMessage(ev, w), "PlayerEvent serializes with stage name");
        ByteReader r(buf, w.size());
        Message out;
        Check(DeserializeMessage(r, out) &&
                  std::strcmp(out.payload.playerEvent.stage, "F_SP108") == 0,
            "v10 PlayerEvent round-trip keeps the stage name");
    }
}

// ---------------------------------------------------------------------------
// Wire determinism (zero-init payload unions)
// ---------------------------------------------------------------------------

/// Two independently-constructed logical messages must produce byte-identical
/// wire output. Before M0.5 the payload union's default ctor did not zero its
/// members, so unset reserved fields serialized stale stack garbage and two
/// runs produced different bytes (deepseek M3).
void RunDeterminismChecks() {
    std::printf("wire: deterministic serialization (zero-init payload unions)\n");
    // v9: sweep the 9-type set — ghosts and time/weather are gone; v11 added HorseState.
    for (u16 t = static_cast<u16>(MsgType::JoinRequest); t <= static_cast<u16>(MsgType::HorseState);
         ++t)
    {
        const auto type = static_cast<MsgType>(t);
        const Message a = MakeMessage(type);
        const Message b = MakeMessage(type);
        u8 bufA[kMaxMessageSize];
        u8 bufB[kMaxMessageSize];
        ByteWriter wa(bufA, sizeof(bufA));
        ByteWriter wb(bufB, sizeof(bufB));
        const bool okA = SerializeMessage(a, wa);
        const bool okB = SerializeMessage(b, wb);
        Check(okA && okB && wa.size() == wb.size() && std::memcmp(bufA, bufB, wa.size()) == 0,
            "identical logical message -> identical wire bytes");
    }
}

// ---------------------------------------------------------------------------
// Semantic validation on receive (deepseek M5)
// ---------------------------------------------------------------------------

void RunValidationChecks() {
    std::printf("validation: semantic checks on receive\n");
    {
        // jointCount == kMaxJoints parses fine.
        const Message ok = MakeMessage(MsgType::PlayerState);
        u8 buf[kMaxMessageSize];
        ByteWriter w(buf, sizeof(buf));
        Check(SerializeMessage(ok, w), "valid PlayerState serializes");
        ByteReader r(buf, w.size());
        Message out;
        Check(DeserializeMessage(r, out), "PlayerState with jointCount == kMaxJoints parses");
    }
    {
        // jointCount beyond the fixed table is rejected at parse, so M1's
        // apply code can never index joints[jointCount] out of bounds.
        Message bad = MakeMessage(MsgType::PlayerState);
        bad.payload.playerState.jointCount = static_cast<u8>(kMaxJoints + 1);
        u8 buf[kMaxMessageSize];
        ByteWriter w(buf, sizeof(buf));
        Check(SerializeMessage(bad, w), "over-range jointCount still serializes (fixed layout)");
        ByteReader r(buf, w.size());
        Message out;
        Check(!DeserializeMessage(r, out), "jointCount > kMaxJoints rejected at parse");
    }
    {
        Message bad = MakeMessage(MsgType::HorseState);
        bad.payload.horseState.jointCount = static_cast<u8>(kMaxJoints + 1);
        u8 buf[kMaxMessageSize];
        ByteWriter w(buf, sizeof(buf));
        Check(SerializeMessage(bad, w), "HorseState over-range jointCount serializes");
        ByteReader r(buf, w.size());
        Message out;
        Check(!DeserializeMessage(r, out), "HorseState jointCount > kMaxJoints rejected");
    }
    {
        Message bad = MakeMessage(MsgType::PlayerState);
        bad.payload.playerState.pos.x = std::numeric_limits<f32>::quiet_NaN();
        u8 buf[kMaxMessageSize];
        ByteWriter w(buf, sizeof(buf));
        Check(SerializeMessage(bad, w), "PlayerState with NaN pos still serializes");
        ByteReader r(buf, w.size());
        Message out;
        Check(!DeserializeMessage(r, out), "PlayerState NaN pos rejected at parse");
    }
    {
        Message bad = MakeMessage(MsgType::PlayerState);
        bad.payload.playerState.joints[0][1][3] = std::numeric_limits<f32>::infinity();
        u8 buf[kMaxMessageSize];
        ByteWriter w(buf, sizeof(buf));
        Check(SerializeMessage(bad, w), "PlayerState with inf joint still serializes");
        ByteReader r(buf, w.size());
        Message out;
        Check(!DeserializeMessage(r, out), "PlayerState inf joint rejected at parse");
    }
    {
        Message bad = MakeMessage(MsgType::HorseState);
        bad.payload.horseState.pos.y = std::numeric_limits<f32>::quiet_NaN();
        u8 buf[kMaxMessageSize];
        ByteWriter w(buf, sizeof(buf));
        Check(SerializeMessage(bad, w), "HorseState with NaN pos still serializes");
        ByteReader r(buf, w.size());
        Message out;
        Check(!DeserializeMessage(r, out), "HorseState NaN pos rejected at parse");
    }
}

// ---------------------------------------------------------------------------
// Ring policies (glm MAJOR-1/MAJOR-2, deepseek M2/m4)
// ---------------------------------------------------------------------------

void RunRingPolicyChecks() {
    std::printf("rings: channel-split overflow policies\n");
    // Reliable ring: drop-newest on full is an EXPLICIT failure (Push false).
    {
        SPSCRing<int, 4> ring;  // capacity 4 -> holds 3 items
        Check(ring.Push(1) && ring.Push(2) && ring.Push(3), "reliable ring accepts up to capacity-1");
        Check(!ring.Push(4), "reliable ring Push fails explicitly when full");
        Check(!ring.Push(5), "reliable ring stays full (still explicit)");
        int v = 0;
        Check(ring.Pop(v) && v == 1, "reliable ring FIFO intact after rejected pushes");
        Check(ring.Pop(v) && v == 2 && ring.Pop(v) && v == 3, "reliable ring drains in order");
        Check(!ring.Pop(v), "reliable ring empty at end");
    }
    // Snapshot ring: replace-newest on full keeps the freshest item.
    {
        SPSCRing<int, 4> ring;
        Check(ring.Push(1) && ring.Push(2), "snapshot ring accepts while not full");
        Check(ring.PushOrReplace(3) == false, "snapshot ring appends while not full");
        Check(ring.PushOrReplace(4) == true, "snapshot ring replaces newest when full");
        Check(ring.PushOrReplace(5) == true, "snapshot ring keeps replacing under sustained pressure");
        int v = 0;
        Check(ring.Pop(v) && v == 1, "replace-newest keeps the oldest item");
        Check(ring.Pop(v) && v == 2 && ring.Pop(v) && v == 5,
            "freshest item (5) wins over the replaced one (4)");
        Check(!ring.Pop(v), "snapshot ring empty at end");
    }
    // Snapshot identity replace: a full ring overwrites the newest matching
    // player, not the newest slot. Otherwise a multiplexed PlayerState for B
    // would drop A's fresh pose to keep a stale A.
    {
        struct Snap {
            int id;
            int seq;
        };
        SPSCRing<Snap, 4> ring;
        auto same = [](const Snap& a, const Snap& b) { return a.id == b.id; };
        Check(ring.Push(Snap{0, 1}) && ring.Push(Snap{1, 1}),
            "identity ring accepts two players");
        Check(ring.PushOrReplaceMatching(Snap{0, 2}, same) == false,
            "identity ring appends player 0 refresh while not full");
        Check(ring.PushOrReplaceMatching(Snap{1, 2}, same) == true,
            "identity ring replaces player 1, not the newest player-0 slot");
        Snap v{};
        Check(ring.Pop(v) && v.id == 0 && v.seq == 1, "oldest player-0 kept");
        Check(ring.Pop(v) && v.id == 1 && v.seq == 2, "player 1 updated in place");
        Check(ring.Pop(v) && v.id == 0 && v.seq == 2, "newest player-0 kept");
        Check(!ring.Pop(v), "identity ring empty");
    }
}

/// Floods the reliable and snapshot outbox rings over a real connection: the
/// accounting must be exact (every rejected Send is counted — no silent
/// drops) and snapshot Sends must never fail.
void RunRingOverflowChecks() {
    std::printf("rings: overflow accounting over real sockets\n");
    auto hostT = std::make_unique<Transport>();
    Check(hostT->StartHost(0), "host transport up");
    const u16 port = hostT->BoundPort();
    auto cliT = std::make_unique<Transport>();
    Check(cliT->StartClient("127.0.0.1", port), "client transport up");
    InboundPacket pkt;
    Check(WaitPoll(*hostT, [](const InboundPacket& p) { return p.type == NetEventType::Connected; },
               10000, pkt),
        "host sees the client connect");
    Check(WaitPoll(*cliT, [](const InboundPacket& p) { return p.type == NetEventType::Connected; },
               10000, pkt),
        "client sees the host connect");

    u8 msg[16];
    std::memset(msg, 0x5A, sizeof(msg));

    // Reliable flood: Send()==false must imply the counter bumped, and vice
    // versa — a reliable event is never silently dropped.
    const u64 dropped0 = cliT->ReliableOutboundDropped();
    u64 ok = 0;
    u64 fail = 0;
    for (int i = 0; i < 20000; ++i) {
        if (cliT->Send(0, kChannelReliable, msg, sizeof(msg))) {
            ++ok;
        } else {
            ++fail;
        }
    }
    const u64 dropped1 = cliT->ReliableOutboundDropped();
    Check(ok + fail == 20000, "every Send returned");
    Check(fail == dropped1 - dropped0,
        "every rejected reliable Send is counted (explicit failure, no silent drop)");
    // Every accepted entry eventually reaches enet_peer_send on the socket
    // thread (the counter is delivery-independent, so no host drain needed).
    {
        const u64 deadline = NowMs() + 15000;
        while (NowMs() < deadline && cliT->PacketsSent() < ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(cliT->PacketsSent() == ok, "every accepted reliable Send was sent on the socket thread");
    }

    // Snapshot flood: replace-newest means Send never fails, and the ring
    // replacement is visible on the producing side (the test thread outruns
    // the socket thread's drain, so the per-peer 128-slot outbox fills).
    u64 snapOk = 0;
    for (int i = 0; i < 20000; ++i) {
        if (cliT->Send(0, kChannelUnreliable, msg, sizeof(msg))) {
            ++snapOk;
        }
    }
    Check(snapOk == 20000, "snapshot Send never fails (replace-newest)");
    // (The HOST-side replace counter is not asserted: ENet drops unreliable
    // packets at the sender once its queue exceeds the packet threshold, so
    // only the freshest reach the host and the 512-slot inbox may never fill.)
    {
        const u64 deadline = NowMs() + 3000;
        while (NowMs() < deadline && cliT->SnapshotOutboundReplaced() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(cliT->SnapshotOutboundReplaced() > 0,
            "snapshot outbox replaced stale entries under flood");
    }

    cliT->Stop();
    hostT->Stop();
}

// ---------------------------------------------------------------------------
// NetClock cadence
// ---------------------------------------------------------------------------

void RunClockChecks() {
    std::printf("clock: 60 Hz send cadence\n");
    {
        NetClock clock(0);
        bool allTick = true;
        for (u64 i = 1; i <= 60; ++i) {
            allTick = allTick && clock.Tick(i * NetClock::kIntervalUs);
        }
        Check(allTick, "one tick per 16667 us sample (60 samples)");
        Check(clock.Frame() == 60, "frame counter == 60");
        Check(!clock.Tick(60 * NetClock::kIntervalUs + 1), "no tick just after the 60th");
    }
    {
        // Samples below the first tick boundary must never tick.
        NetClock clock(1000000);  // first tick due at 1,000,000 + kIntervalUs
        bool any = false;
        for (u64 i = 0; i < 1000; ++i) {
            any = any || clock.Tick(1000000 + i);  // 1 us apart, all below the boundary
        }
        Check(!any && clock.Frame() == 0, "no ticks at sub-interval spacing");
        Check(clock.Tick(1000000 + NetClock::kIntervalUs),
            "the first tick fires exactly at the boundary");
    }
    {
        NetClock clock(0);
        Check(clock.Tick(1000000), "stall of ~1 s ticks once on catch-up");
        Check(clock.Frame() == 1, "catch-up does not burst frames");
        const u64 next = 61 * NetClock::kIntervalUs;
        Check(clock.Tick(next), "cadence resumes at the next boundary");
        Check(!clock.Tick(next + 1), "and stays quiet just after");
    }
}

// ---------------------------------------------------------------------------
// LAN handshake demo (in-process host + clients over loopback)
// ---------------------------------------------------------------------------

/// Pumps every live session until `pred` holds or the deadline expires.
struct Demo {
    std::vector<Session*> live;

    bool WaitFor(const std::function<bool()>& pred, u64 timeoutMs) {
        const u64 deadline = NowMs() + timeoutMs;
        while (NowMs() < deadline) {
            for (Session* s : live) {
                s->Update();
            }
            if (pred()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
};

u8 PresentCountOf(const Session& s) {
    u8 count = 0;
    for (const auto& slot : s.roster()) {
        if (slot.present) {
            ++count;
        }
    }
    return count;
}

void RunHandshakeDemo() {
    std::printf("handshake: LAN host/client over 127.0.0.1\n");
    Demo demo;

    // -- host --
    auto host = std::make_unique<Session>();
    SessionConfig hostCfg;
    hostCfg.port = 0;  // ephemeral; the demo reads boundPort()
    hostCfg.name = "Test Host";
    hostCfg.maxPlayers = 2;
    std::strncpy(hostCfg.stage.stage, "F_SP108", sizeof(hostCfg.stage.stage) - 1);
    hostCfg.stage.room = 2;
    hostCfg.stage.layer = 1;
    hostCfg.stage.point = 7;
    Check(host->StartHost(hostCfg), "host session starts (Listening)");
    demo.live.push_back(host.get());
    const u16 port = host->boundPort();
    Check(port != 0, "host bound an ephemeral port");
    std::printf("    host listening on 127.0.0.1:%u\n", port);

    // -- client A joins --
    {
        auto a = std::make_unique<Session>();
        SessionConfig cfg;
        cfg.joinHost = "127.0.0.1";
        cfg.port = port;
        cfg.name = "Player A";
        cfg.version = kProtocolVersion;
        Check(a->StartClient(cfg), "client A starts (Connecting)");
        demo.live.push_back(a.get());

        Check(demo.WaitFor([&] { return a->state() == SessionState::Joined; }, 10000),
            "A reaches Joined (JoinRequest -> JoinAccept)");
        Check(a->selfId() == 1, "A assigned PlayerId 1");
        Check(demo.WaitFor([&] { return PresentCountOf(*host) == 2; }, 10000),
            "host roster has 2 players (host + A)");
        Check(std::strcmp(a->worldStage().stage, "F_SP108") == 0 && a->worldStage().room == 2 &&
                  a->worldStage().point == 7,
            "A received WorldInit stage/room/point from the host");
        Check(a->roster()[0].present && std::strcmp(a->roster()[0].name, "Test Host") == 0,
            "A sees the host in the roster");

        // -- client B: wrong protocol version -> reject --
        auto b = std::make_unique<Session>();
        SessionConfig bCfg;
        bCfg.joinHost = "127.0.0.1";
        bCfg.port = port;
        bCfg.name = "Player B (old)";
        bCfg.version = kProtocolVersion + 100;
        Check(b->StartClient(bCfg), "client B starts");
        demo.live.push_back(b.get());
        Check(demo.WaitFor([&] { return b->state() == SessionState::Rejected; }, 10000),
            "B rejected (version mismatch)");
        Check(std::strstr(b->rejectReasonName(), "version") != nullptr,
            "B rejection reason is version mismatch");
        b->Stop();
        demo.live.erase(std::remove(demo.live.begin(), demo.live.end(), b.get()), demo.live.end());

        // -- client C: roster full (maxPlayers=2) -> reject --
        auto c = std::make_unique<Session>();
        SessionConfig cCfg;
        cCfg.joinHost = "127.0.0.1";
        cCfg.port = port;
        cCfg.name = "Player C";
        cCfg.version = kProtocolVersion;
        Check(c->StartClient(cCfg), "client C starts");
        demo.live.push_back(c.get());
        Check(demo.WaitFor([&] { return c->state() == SessionState::Rejected; }, 10000),
            "C rejected (session full)");
        Check(std::strstr(c->rejectReasonName(), "full") != nullptr,
            "C rejection reason is session full");
        c->Stop();
        demo.live.erase(std::remove(demo.live.begin(), demo.live.end(), c.get()), demo.live.end());

        // -- A leaves: PlayerLeave relayed, host roster shrinks --
        Check(a->state() == SessionState::Joined, "A still joined before leaving");
        a->Stop();
        Check(a->state() == SessionState::Ended, "A session Ended after Stop");
        Check(demo.WaitFor([&] { return PresentCountOf(*host) == 1; }, 10000),
            "host roster back to 1 (host) after A's PlayerLeave");
        demo.live.erase(std::remove(demo.live.begin(), demo.live.end(), a.get()), demo.live.end());
    }

    // -- client D joins, then the host ends the session --
    {
        auto d = std::make_unique<Session>();
        SessionConfig cfg;
        cfg.joinHost = "127.0.0.1";
        cfg.port = port;
        cfg.name = "Player D";
        cfg.version = kProtocolVersion;
        Check(d->StartClient(cfg), "client D starts");
        demo.live.push_back(d.get());
        Check(demo.WaitFor([&] { return d->state() == SessionState::Joined; }, 10000),
            "D reaches Joined (PlayerId reuses slot 1)");
        Check(d->selfId() == 1, "D assigned PlayerId 1");

        host->Stop();
        Check(host->state() == SessionState::Ended, "host session Ended after Stop");
        Check(demo.WaitFor([&] { return d->state() == SessionState::Ended; }, 10000),
            "D observes SessionEnd(HostLeft) and goes Ended");
        d->Stop();
        demo.live.erase(std::remove(demo.live.begin(), demo.live.end(), d.get()), demo.live.end());
    }

    demo.live.erase(std::remove(demo.live.begin(), demo.live.end(), host.get()), demo.live.end());
    Check(host->sessionFrames() > 0, "host session pumped frames");
}

/// Game-message routing (M1 integration seam): clients send PlayerState/
/// PlayerEvent into the session; the host applies them locally and relays
/// them to every other joined peer (star topology, 00-network.md §2).
void RunGameMessageDemo() {
    std::printf("handshake: game-message routing (host relay)\n");
    Demo demo;

    auto host = std::make_unique<Session>();
    SessionConfig hostCfg;
    hostCfg.port = 0;
    hostCfg.name = "Relay Host";
    hostCfg.maxPlayers = 3;
    Check(host->StartHost(hostCfg), "relay host starts (Listening)");
    demo.live.push_back(host.get());
    const u16 port = host->boundPort();

    auto a = std::make_unique<Session>();
    SessionConfig aCfg;
    aCfg.joinHost = "127.0.0.1";
    aCfg.port = port;
    aCfg.name = "Player A";
    aCfg.version = kProtocolVersion;
    Check(a->StartClient(aCfg), "relay client A starts");
    demo.live.push_back(a.get());
    Check(demo.WaitFor([&] { return a->state() == SessionState::Joined; }, 10000), "A joined");

    auto b = std::make_unique<Session>();
    SessionConfig bCfg;
    bCfg.joinHost = "127.0.0.1";
    bCfg.port = port;
    bCfg.name = "Player B";
    bCfg.version = kProtocolVersion;
    Check(b->StartClient(bCfg), "relay client B starts");
    demo.live.push_back(b.get());
    Check(demo.WaitFor([&] { return b->state() == SessionState::Joined; }, 10000), "B joined");
    Check(demo.WaitFor([&] { return PresentCountOf(*host) == 3; }, 10000),
        "host roster has 3 players");
    // Roster-refresh broadcast (MAJOR M1): A joined before B, so A must learn
    // about B through the WorldInit the host re-broadcasts on B's join (A's
    // roster never had B otherwise — join and leave were asymmetric).
    Check(demo.WaitFor([&] { return a->roster()[b->selfId()].present; }, 10000),
        "A's roster shows B after B joined (WorldInit roster-refresh)");
    Check(b->roster()[a->selfId()].present && b->roster()[0].present,
        "B's roster shows A and the host");
    Check(!a->roster()[b->selfId() + 1].present,
        "A's roster has no phantom players beyond the roster");

    int hostStates = 0;
    int aStates = 0;
    int bStates = 0;
    int hostEvents = 0;
    int aEvents = 0;
    u8 hostStatePid = kInvalidPlayerId;
    u8 bStatePid = kInvalidPlayerId;
    int hostHorses = 0;
    int bHorses = 0;
    u8 hostHorsePid = kInvalidPlayerId;
    u8 bHorsePid = kInvalidPlayerId;
    host->SetGameMessageHandler([&](MsgType type, const PayloadUnion& p) {
        if (type == MsgType::PlayerState) {
            ++hostStates;
            hostStatePid = p.playerState.playerId;
        } else if (type == MsgType::PlayerEvent) {
            ++hostEvents;
        } else if (type == MsgType::HorseState) {
            ++hostHorses;
            hostHorsePid = p.horseState.playerId;
        }
    });
    a->SetGameMessageHandler([&](MsgType type, const PayloadUnion& p) {
        if (type == MsgType::PlayerState) {
            ++aStates;
        } else if (type == MsgType::PlayerEvent) {
            ++aEvents;
        }
    });
    b->SetGameMessageHandler([&](MsgType type, const PayloadUnion& p) {
        if (type == MsgType::PlayerState) {
            ++bStates;
            bStatePid = p.playerState.playerId;
        } else if (type == MsgType::HorseState) {
            ++bHorses;
            bHorsePid = p.horseState.playerId;
        }
    });

    // A sends a PlayerState: the host consumes it locally and relays it to B.
    PayloadUnion ps = {};
    ps.playerState.playerId = a->selfId();
    ps.playerState.pos = Vec3f{1.0f, 2.0f, 3.0f};
    Check(a->SendGameMessage(MsgType::PlayerState, ps), "A sends PlayerState");
    Check(demo.WaitFor([&] { return hostStates >= 1 && bStates >= 1; }, 10000),
        "host consumed A's PlayerState and relayed it to B");
    Check(hostStatePid == a->selfId() && bStatePid == a->selfId(),
        "relayed PlayerState keeps the source playerId");
    Check(aStates == 0, "A never receives its own PlayerState back");

    // Host's own PlayerState reaches both clients.
    PayloadUnion hostPs = {};
    hostPs.playerState.playerId = 0;
    Check(host->SendGameMessage(MsgType::PlayerState, hostPs), "host sends PlayerState");
    Check(demo.WaitFor([&] { return aStates >= 1 && bStates >= 2; }, 10000),
        "clients received the host's PlayerState");

    // B's PlayerEvent is relayed to A (and consumed by the host).
    PayloadUnion ev = {};
    ev.playerEvent.playerId = b->selfId();
    ev.playerEvent.eventId = static_cast<u8>(PlayerEventId::FormChange);
    ev.playerEvent.data = 1;
    Check(b->SendGameMessage(MsgType::PlayerEvent, ev), "B sends PlayerEvent");
    Check(demo.WaitFor([&] { return hostEvents >= 1 && aEvents >= 1; }, 10000),
        "host relayed B's PlayerEvent to A");

    PayloadUnion hs = {};
    hs.horseState.playerId = a->selfId();
    hs.horseState.jointCount = 38;
    hs.horseState.pos = Vec3f{10.0f, 20.0f, 30.0f};
    Check(a->SendGameMessage(MsgType::HorseState, hs), "A sends HorseState");
    Check(demo.WaitFor([&] { return hostHorses >= 1 && bHorses >= 1; }, 10000),
        "host consumed A's HorseState and relayed it to B");
    Check(hostHorsePid == a->selfId() && bHorsePid == a->selfId(),
        "relayed HorseState keeps the source playerId");

    // SendGameMessage is refused outside a playable session state.
    auto idle = std::make_unique<Session>();
    Check(!idle->SendGameMessage(MsgType::PlayerState, ps),
        "SendGameMessage refused while idle");

    for (Session* s : demo.live) {
        s->Stop();
    }
    demo.live.clear();
    host->Stop();
}


// ---------------------------------------------------------------------------
// Peer-slot generation guard (deepseek M4)
// ---------------------------------------------------------------------------

/// connect -> disconnect -> connect again must never deliver a stale packet
/// from the old connection to the one that reuses the slot. Deterministic:
/// A sends a snapshot while owning slot 0, then disconnects GRACEFULLY (a
/// hard enet_host_destroy never notifies the peer — ENet destroys the socket
/// before flushing the disconnect command, so the host would only free the
/// slot after its ~5 s peer timeout). The generation bumps on reassign; A's
/// stale snapshot is dropped at Poll time and D's fresh traffic flows
/// normally.
void RunGenerationGuardCheck() {
    std::printf("generation: no stale delivery across peer-slot reuse\n");
    auto hostT = std::make_unique<Transport>();
    Check(hostT->StartHost(0), "host transport up");
    const u16 port = hostT->BoundPort();

    // A: a raw ENet peer driven by the test thread, so the disconnect is a
    // controlled, acknowledged handshake rather than a silent socket close.
    ENetHost* aRaw = enet_host_create(nullptr, 1, 2, 0, 0);
    Check(aRaw != nullptr, "raw A host up");
    ENetAddress addr;
    Check(enet_address_set_host(&addr, "127.0.0.1") == 0, "A resolves 127.0.0.1");
    addr.port = port;
    ENetPeer* aPeer = enet_host_connect(aRaw, &addr, 2, 0);
    Check(aPeer != nullptr, "raw A connects");
    {
        // A must be serviced from the test thread AND the host transport
        // polled in the same loop, or the ENet connect handshake stalls.
        const u64 deadline = NowMs() + 10000;
        bool hostSeen = false;
        bool aSeen = false;
        while (NowMs() < deadline && (!hostSeen || !aSeen)) {
            ENetEvent ev;
            while (enet_host_service(aRaw, &ev, 0) > 0) {
                if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                    aSeen = true;
                }
            }
            InboundPacket p;
            while (hostT->Poll(p)) {
                if (p.type == NetEventType::Connected && p.peerIndex == 0) {
                    hostSeen = true;
                }
            }
            if (!hostSeen || !aSeen) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        Check(hostSeen && aSeen, "A connected on both sides (host peer slot 0)");

        // A sends a distinctive snapshot, then disconnects gracefully.
        // Loopback ordering guarantees the host processes the snapshot before
        // the disconnect.
        const u8 stale[] = {'S', 'T', 'A', 'L', 'E'};
        ENetPacket* sp = enet_packet_create(stale, sizeof(stale), 0);
        Check(enet_peer_send(aPeer, kChannelUnreliable, sp) == 0, "A sends its snapshot");
        enet_host_flush(aRaw);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        enet_peer_disconnect(aPeer, 0);
        const u64 deadline2 = NowMs() + 10000;
        bool aGone = false;
        while (NowMs() < deadline2 && !aGone) {
            ENetEvent ev;
            while (enet_host_service(aRaw, &ev, 0) > 0) {
                if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                    aGone = true;
                    break;
                }
            }
            if (!aGone) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        Check(aGone, "A's disconnect acknowledged (host released the slot)");
    }
    enet_host_destroy(aRaw);

    // D reconnects onto the freed slot.
    {
        auto dT = std::make_unique<Transport>();
        Check(dT->StartClient("127.0.0.1", port), "client D transport connects (slot reuse)");
        InboundPacket pkt;
        Check(WaitPoll(*hostT,
                   [](const InboundPacket& p) {
                       return p.type == NetEventType::Connected && p.peerIndex == 0;
                   },
                   10000, pkt),
            "host sees D connected on the reused slot 0");
        // Drain whatever remains after D occupies the slot. A's snapshot
        // must not be delivered as if it came from D. It may already have
        // been consumed as A's last packet (generation is not bumped on
        // release — JoinReject must still land) or dropped here by the
        // assign-time generation bump.
        bool sawStale = false;
        InboundPacket p;
        while (hostT->Poll(p)) {
            if (p.type == NetEventType::Data) {
                sawStale = sawStale || (p.size == 5 && std::memcmp(p.data, "STALE", 5) == 0);
            }
        }
        Check(!sawStale, "host never delivered A's stale snapshot to D");

        // D's fresh snapshot crosses the same slot fine.
        const u8 fresh[] = {'F', 'R', 'E', 'S', 'H'};
        Check(dT->Send(0, kChannelUnreliable, fresh, sizeof(fresh)), "D enqueues a fresh snapshot");
        Check(WaitPoll(*hostT,
                   [](const InboundPacket& p) {
                       return p.type == NetEventType::Data && p.size == 5 &&
                              std::memcmp(p.data, "FRESH", 5) == 0;
                   },
                   10000, pkt),
            "host receives D's fresh snapshot on the reused slot");
        Check(pkt.peerIndex == 0 && pkt.channel == kChannelUnreliable,
            "fresh snapshot attributed to peer slot 0 on the snapshot channel");
        dT->Stop();
    }
    hostT->Stop();
}

// ---------------------------------------------------------------------------
// Duplicate JoinRequest guard (deepseek m2)
// ---------------------------------------------------------------------------

/// A raw protocol client sends JoinRequest twice from the same ENet peer; the
/// host must not assign a second PlayerId.
void RunDuplicateJoinCheck() {
    std::printf("join: duplicate JoinRequest is ignored\n");
    auto host = std::make_unique<Session>();
    SessionConfig hc;
    hc.port = 0;
    hc.name = "DupHost";
    hc.maxPlayers = 4;
    Check(host->StartHost(hc), "host session up (maxPlayers 4)");
    const u16 port = host->boundPort();

    // Raw transport "client" that speaks the wire protocol but not the
    // session machine — it can send JoinRequest as many times as it likes.
    auto rogue = std::make_unique<Transport>();
    Check(rogue->StartClient("127.0.0.1", port), "rogue client transport connects");
    InboundPacket pkt;
    Check(WaitPoll(*rogue, [](const InboundPacket& p) { return p.type == NetEventType::Connected; },
               10000, pkt),
        "rogue sees Connected");

    Message jr = {};
    jr.type = MsgType::JoinRequest;
    jr.payload.joinRequest.version = kProtocolVersion;
    jr.payload.joinRequest.requestedSlot = kAnySlot;
    std::strncpy(jr.payload.joinRequest.name, "Rogue", sizeof(jr.payload.joinRequest.name) - 1);
    u8 buf[kMaxMessageSize];
    ByteWriter w(buf, sizeof(buf));
    Check(SerializeMessage(jr, w), "rogue JoinRequest serializes");

    Check(rogue->Send(0, kChannelReliable, buf, w.size()), "rogue sends JoinRequest #1");
    {
        const u64 deadline = NowMs() + 10000;
        bool joined = false;
        while (NowMs() < deadline) {
            host->Update();
            if (PresentCountOf(*host) == 2) {
                joined = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(joined, "host assigned player 1 to the rogue (roster == 2)");
    }
    Check(host->roster()[1].present, "rogue holds PlayerId 1");

    // Same peer sends JoinRequest again; the roster must not grow.
    Check(rogue->Send(0, kChannelReliable, buf, w.size()), "rogue sends JoinRequest #2");
    {
        const u64 before = PresentCountOf(*host);
        const u64 deadline = NowMs() + 1000;
        while (NowMs() < deadline) {
            host->Update();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(PresentCountOf(*host) == before, "duplicate JoinRequest ignored — no second slot");
        Check(!host->roster()[2].present && !host->roster()[3].present, "slots 2/3 stay free");
    }

    rogue->Stop();
    host->Stop();
}

// ---------------------------------------------------------------------------
// JoinAccept semantic validation (deepseek M5)
// ---------------------------------------------------------------------------

/// A fake host (raw transport) forges JoinAccepts that the client session
/// must reject: an out-of-range assignedPlayerId, and a valid id whose roster
/// slot is not marked present.
void RunJoinAcceptValidationCheck() {
    std::printf("validation: forged JoinAccept rejected by the client session\n");
    auto fakeHost = std::make_unique<Transport>();
    Check(fakeHost->StartHost(0), "fake host transport up");
    const u16 port = fakeHost->BoundPort();
    InboundPacket pkt;

    for (int caseNo = 0; caseNo < 2; ++caseNo) {
        auto c = std::make_unique<Session>();
        SessionConfig cfg;
        cfg.joinHost = "127.0.0.1";
        cfg.port = port;
        cfg.name = "Victim";
        Check(c->StartClient(cfg), "client session starts");
        // Note: each case's client stops with a HARD transport teardown, so
        // the fake host does not free the peer slot until ENet's ~5 s peer
        // timeout — later cases land on higher slots. Always send to the
        // slot captured from the connect event.
        Check(WaitPoll(*fakeHost, [](const InboundPacket& p) { return p.type == NetEventType::Connected; },
                   10000, pkt),
            "fake host sees the client connect");
        c->Update();  // let the client process its own Connected (sends JoinRequest, ignored)

        Message accept = MakeMessage(MsgType::JoinAccept);
        if (caseNo == 0) {
            accept.payload.joinAccept.assignedPlayerId = 99;  // out of range
        } else {
            accept.payload.joinAccept.assignedPlayerId = 2;   // in range but...
            for (auto& e : accept.payload.joinAccept.roster) {
                e.present = 0;  // ...the roster does not back the assignment
            }
        }
        u8 buf[kMaxMessageSize];
        ByteWriter w(buf, sizeof(buf));
        Check(SerializeMessage(accept, w), "forged JoinAccept serializes");
        Check(fakeHost->Send(pkt.peerIndex, kChannelReliable, buf, w.size()),
            "fake host sends the forged JoinAccept");
        Check(WaitSessionState(*c, SessionState::Rejected, 10000),
            caseNo == 0 ? "client rejected out-of-range assignedPlayerId"
                        : "client rejected roster-not-present assignment");
        Check(std::strstr(c->rejectReasonName(), "invalid") != nullptr,
            "rejection reason is 'invalid join accept'");
        c->Stop();
    }
    fakeHost->Stop();
}

// ---------------------------------------------------------------------------
// Oversized inbound metric (GLM MINOR-5)
// ---------------------------------------------------------------------------

/// A raw ENet client (not the Transport — its Send() rejects oversized sizes
/// at the API) pushes a packet larger than kMaxMessageSize; the host
/// transport must drop it and count it as InboundOversized.
void RunOversizedMetricCheck() {
    std::printf("metric: oversized inbound packets counted\n");
    auto hostT = std::make_unique<Transport>();
    Check(hostT->StartHost(0), "host transport up");
    const u16 port = hostT->BoundPort();

    ENetHost* raw = enet_host_create(nullptr, 1, 2, 0, 0);
    Check(raw != nullptr, "raw ENet client host up");
    ENetAddress addr;
    Check(enet_address_set_host(&addr, "127.0.0.1") == 0, "raw client resolves 127.0.0.1");
    addr.port = port;
    ENetPeer* rawPeer = enet_host_connect(raw, &addr, 2, 0);
    Check(rawPeer != nullptr, "raw client connects");
    {
        const u64 deadline = NowMs() + 10000;
        bool connected = false;
        while (NowMs() < deadline && !connected) {
            ENetEvent ev;
            while (enet_host_service(raw, &ev, 0) > 0) {
                if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                    connected = true;
                    break;
                }
            }
            if (!connected) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        Check(connected, "raw client connected");
    }

    std::vector<u8> big(kMaxMessageSize + 1, 0xCD);
    ENetPacket* bigPkt = enet_packet_create(big.data(), big.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(rawPeer, 0, bigPkt);
    enet_host_flush(raw);

    InboundPacket p;
    const u64 deadline = NowMs() + 10000;
    while (NowMs() < deadline && hostT->InboundOversized() == 0) {
        while (hostT->Poll(p)) {  // drain the connect event etc.
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(hostT->InboundOversized() >= 1, "oversized inbound packet dropped and counted");

    enet_peer_reset(rawPeer);
    enet_host_destroy(raw);
    hostT->Stop();
}

// ---------------------------------------------------------------------------
// M4 — room ownership (network.md §6)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// M4/M4.5 — worldStage carry (stay-put join) + entity stability
// ---------------------------------------------------------------------------

/// M4.5 (MAJOR 1 — user decision): the join-warp machinery was REMOVED; a
/// joining client boots into its own save stage and stays put (players meet
/// by traveling). What remains from the join-warp work is the session-level
/// worldStage carry — the host fills worldStage_ from the real Link every
/// frame and JoinAccept/WorldInit carry it to the client (the joiner's
/// "where is the host" reference; the cross-stage `stageOk` puppet gate keys
/// on the remote's REAL stage from PlayerState, so puppets stay hidden until
/// both players share a stage).
void RunM4WorldStageCheck() {
    std::printf("m4: worldStage carry (stay-put join policy, D6 revised)\n");

    // Session-level: the host fills worldStage_ (the M1 TODO — cfg.stage
    // was inert) and JoinAccept/WorldInit carry it to the client. (The
    // join-warp decision table itself was removed with the feature.)
    {
        Demo demo;
        auto host = std::make_unique<Session>();
        SessionConfig hostCfg;
        hostCfg.port = 0;
        hostCfg.name = "Warp Host";
        hostCfg.maxPlayers = 2;
        Check(host->StartHost(hostCfg), "host starts and will publish worldStage");
        demo.live.push_back(host.get());
        const u16 port = host->boundPort();

        StageInfo hostStage;
        std::strncpy(hostStage.stage, "F_SP103", sizeof(hostStage.stage) - 1);
        hostStage.room = 4;
        hostStage.layer = -1;
        hostStage.point = 3;
        host->setWorldStage(hostStage);

        auto c = std::make_unique<Session>();
        SessionConfig cCfg;
        cCfg.joinHost = "127.0.0.1";
        cCfg.port = port;
        cCfg.name = "Warp Client";
        cCfg.version = kProtocolVersion;
        Check(c->StartClient(cCfg), "client starts");
        demo.live.push_back(c.get());
        Check(demo.WaitFor([&] { return c->state() == SessionState::Joined; }, 10000),
            "client joined");
        Check(std::strcmp(c->worldStage().stage, "F_SP103") == 0 && c->worldStage().room == 4,
            "client received the host's filled worldStage (stage+room)");
        Check(std::strcmp(host->worldStage().stage, "F_SP103") == 0,
            "host retains its published worldStage");

        for (Session* s : demo.live) {
            s->Stop();
        }
        demo.live.clear();
        host->Stop();
    }
}

// ---------------------------------------------------------------------------
// M4.6 — capstone MAJOR 1: session restart after a remote-side end
// ---------------------------------------------------------------------------

/// Capstone MAJOR 1 (review-full-deepseek-v4-flash-0731.md MAJOR 1): a client
/// whose session ended from the host's side (SessionEnd or connection loss)
/// left its ENet transport running forever — Session::Stop() early-returned
/// for Ended, so the next StartClient/StartHost hit "already-running
/// transport" until an app restart. The fix: Stop() always stops the
/// transport when it is running, independent of state_.
///
/// This test: handshake -> the host ends the session (graceful SessionEnd,
/// then a hard connection-loss via peer timeout) -> the client session is
/// Ended with its transport STILL running -> Stop() joins the transport -> a
/// second session on the SAME Session object in the SAME process starts and
/// joins cleanly.
void RunM46SessionRestartCheck() {
    std::printf("m4.6: session restart after a remote-side end (capstone MAJOR 1)\n");
    Demo demo;

    // -- leg A: graceful host leave (SessionEnd) ---------------------------
    {
        auto hostA = std::make_unique<Session>();
        SessionConfig hcfg;
        hcfg.port = 0;
        hcfg.name = "M46 Host A";
        hcfg.maxPlayers = 2;
        Check(hostA->StartHost(hcfg), "host A starts (Listening)");
        demo.live.push_back(hostA.get());
        const u16 portA = hostA->boundPort();

        auto c = std::make_unique<Session>();
        SessionConfig ccfg;
        ccfg.joinHost = "127.0.0.1";
        ccfg.port = portA;
        ccfg.name = "M46 Client";
        ccfg.version = kProtocolVersion;
        Check(c->StartClient(ccfg), "client starts (Connecting)");
        demo.live.push_back(c.get());
        Check(demo.WaitFor([&] { return c->state() == SessionState::Joined; }, 10000),
            "client joined host A");
        Check(c->transportRunning(), "client transport running while joined");

        // The host ends the session from ITS side: the client goes Ended with
        // NO transport teardown (the pre-fix dead-end — the host's SessionEnd
        // arrives via OnSessionEnd; a hard kill arrives via HandleDisconnect;
        // both leave state_ = Ended with the transport running).
        hostA->Stop();
        demo.live.erase(std::remove(demo.live.begin(), demo.live.end(), hostA.get()),
            demo.live.end());
        Check(demo.WaitFor([&] { return c->state() == SessionState::Ended; }, 10000),
            "client session Ended (host-side end)");
        Check(c->transportRunning(),
            "client transport STILL running after the remote-side end (pre-teardown)");

        // Stop() must tear the transport down even though state_ == Ended.
        c->Stop();
        Check(!c->transportRunning(), "Stop() stopped the transport after a remote-side end");
        Check(c->state() == SessionState::Ended, "session state stays Ended after Stop");

        // A second session on the same Session object in the same process.
        auto hostB = std::make_unique<Session>();
        SessionConfig h2;
        h2.port = 0;
        h2.name = "M46 Host B";
        h2.maxPlayers = 2;
        Check(hostB->StartHost(h2), "host B starts (Listening)");
        demo.live.push_back(hostB.get());
        const u16 portB = hostB->boundPort();

        ccfg.port = portB;  // point the client at host B, not the dead host A
        Check(c->StartClient(ccfg),
            "client restarts against host B (second session, same process)");
        Check(demo.WaitFor([&] { return c->state() == SessionState::Joined; }, 10000),
            "client reached Joined in the second session");
        Check(c->selfId() == 1, "client re-assigned a PlayerId in the second session");
        Check(c->transportRunning(), "second-session transport running");

        c->Stop();
        demo.live.erase(std::remove(demo.live.begin(), demo.live.end(), c.get()), demo.live.end());
        hostB->Stop();
        demo.live.erase(std::remove(demo.live.begin(), demo.live.end(), hostB.get()),
            demo.live.end());
    }

    // -- leg B: hard connection loss (HandleDisconnect via peer timeout) ---
    {
        // A raw transport acts as the "host": it completes the ENet
        // handshake and answers the JoinRequest with a VALID JoinAccept, so
        // the client session reaches Joined; then its transport dies WITHOUT
        // a SessionEnd (as in a crash). The client detects the dead peer via
        // the ENet peer timeout (~5 s) and HandleDisconnect flips it to Ended
        // with endReason ConnectionLost.
        auto fakeHost = std::make_unique<Transport>();
        Check(fakeHost->StartHost(0), "fake host transport up (crash victim)");
        const u16 port = fakeHost->BoundPort();
        InboundPacket pkt;

        auto c = std::make_unique<Session>();
        SessionConfig ccfg;
        ccfg.joinHost = "127.0.0.1";
        ccfg.port = port;
        ccfg.name = "M46 Crash Victim";
        ccfg.version = kProtocolVersion;
        Check(c->StartClient(ccfg), "crash-victim client starts (Connecting)");
        demo.live.push_back(c.get());
        Check(WaitPoll(*fakeHost,
                   [](const InboundPacket& p) { return p.type == NetEventType::Connected; },
                   10000, pkt),
            "fake host sees the client connect");
        c->Update();  // client processes Connected and sends JoinRequest

        // Fake host replies with a valid JoinAccept (id 1 backed by roster).
        Message accept = MakeMessage(MsgType::JoinAccept);
        accept.payload.joinAccept.assignedPlayerId = 1;
        for (auto& e : accept.payload.joinAccept.roster) {
            e = {};
        }
        accept.payload.joinAccept.roster[0].present = 1;
        accept.payload.joinAccept.roster[0].playerId = 0;
        std::strncpy(accept.payload.joinAccept.roster[0].name, "Crash Host",
            sizeof(accept.payload.joinAccept.roster[0].name) - 1);
        accept.payload.joinAccept.roster[1].present = 1;
        accept.payload.joinAccept.roster[1].playerId = 1;
        u8 buf[kMaxMessageSize];
        ByteWriter w(buf, sizeof(buf));
        Check(SerializeMessage(accept, w), "fake JoinAccept serializes");
        Check(fakeHost->Send(pkt.peerIndex, kChannelReliable, buf, w.size()),
            "fake host sends the valid JoinAccept");
        Check(demo.WaitFor([&] { return c->state() == SessionState::Joined; }, 10000),
            "crash-victim client reached Joined (JoinAccept accepted)");

        // The host transport dies with no SessionEnd; the client's ENet peer
        // times out and HandleDisconnect fires -> Ended (ConnectionLost).
        fakeHost->Stop();
        Check(demo.WaitFor([&] { return c->state() == SessionState::Ended; }, 15000),
            "crash-victim client Ended via connection loss (peer timeout)");
        Check(c->endReason() == SessionEndReason::ConnectionLost,
            "end reason is ConnectionLost (HandleDisconnect path)");
        Check(c->transportRunning(),
            "crash-victim transport STILL running after the connection loss (pre-teardown)");

        // Stop() tears it down; a third session in the same process works.
        c->Stop();
        Check(!c->transportRunning(),
            "Stop() stopped the crash-victim transport after ConnectionLost");

        auto hostC = std::make_unique<Session>();
        SessionConfig h3;
        h3.port = 0;
        h3.name = "M46 Host C";
        h3.maxPlayers = 2;
        Check(hostC->StartHost(h3), "host C starts (Listening)");
        demo.live.push_back(hostC.get());
        const u16 portC = hostC->boundPort();
        ccfg.port = portC;  // point the crash victim at host C
        Check(c->StartClient(ccfg),
            "crash-victim client restarts (third session, same process)");
        Check(demo.WaitFor([&] { return c->state() == SessionState::Joined; }, 10000),
            "crash-victim client joined host C after a connection-loss end");
        c->Stop();
        demo.live.erase(std::remove(demo.live.begin(), demo.live.end(), c.get()), demo.live.end());
        hostC->Stop();
        demo.live.erase(std::remove(demo.live.begin(), demo.live.end(), hostC.get()),
            demo.live.end());
    }
}

void RunSessionStartGuardCheck() {
    std::printf("session: start guards (empty join host, host:port refused)\n");
    {
        Session cli;
        SessionConfig cfg;
        cfg.joinHost = "   ";
        Check(!cli.StartClient(cfg), "StartClient refuses whitespace-only join host");
        Check(std::strcmp(cli.startFailureReason(), "join host is empty") == 0,
            "empty join-host failure reason");
    }
    {
        Session cli;
        SessionConfig cfg;
        cfg.joinHost = "127.0.0.1:44770";
        cfg.port = 44770;
        Check(!cli.StartClient(cfg), "StartClient refuses host:port in joinHost");
        Check(std::strstr(cli.startFailureReason(), "net.hostPort") != nullptr,
            "host:port failure reason points at Host Port");
    }
    {
        Session host;
        SessionConfig hcfg;
        hcfg.port = 0;
        Check(host.StartHost(hcfg), "ephemeral StartHost still allowed");
        const u16 port = host.boundPort();
        Check(port != 0, "ephemeral bind is not port 0");
        Session cli;
        SessionConfig ccfg;
        ccfg.joinHost = "127.0.0.1";
        ccfg.port = port;
        Check(cli.StartClient(ccfg), "StartClient accepts IP + port fields");
        cli.Stop();
        host.Stop();
    }
}

void RunConnectingTimeoutCheck() {
    std::printf("session: Connecting deadline ends an unreachable host\n");
    Session cli;
    SessionConfig cfg;
    cfg.joinHost = "192.0.2.1";  // TEST-NET-1, not a local listener
    cfg.port = 44770;
    cfg.joinTimeoutMs = 250;
    Check(cli.StartClient(cfg), "client starts Connecting to unroutable host");
    Check(cli.state() == SessionState::Connecting, "state is Connecting");
    const u64 deadline = NowMs() + 2000;
    while (NowMs() < deadline && cli.state() == SessionState::Connecting) {
        cli.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Check(cli.state() == SessionState::Ended, "Connecting deadline (or RST) ends the session");
    Check(cli.startFailureReason() != nullptr && cli.startFailureReason()[0] != '\0',
        "pre-join loss sets a start-failure reason");
    Check(!cli.transportRunning(), "transport is torn down");
    cli.Stop();
}

// --------------------------------------------------------------------------
// v9 — compact ids + off-session send refuse
// --------------------------------------------------------------------------

/// Protocol v11 is 9 types (JoinRequest..HorseState); v9 deleted
/// GhostSnapshot/EnemyEvent/time/weather. An idle session refuses every game
/// send. v10 only grew PlayerEvent's payload (stage name). v11 added HorseState.
void RunV9WireRegression() {
    std::printf("v11: compact ids, HorseState, dead ghost/combat/time wire, off-session refuse\n");
    Check(kProtocolVersion == 11, "protocol version is v11");
    Check(static_cast<u16>(MsgType::JoinRequest) == 1 &&
              static_cast<u16>(MsgType::HorseState) == 9,
        "message ids compact 1..9");

    Session idle;
    PayloadUnion pose = {};
    Check(!idle.SendGameMessage(MsgType::PlayerState, pose),
        "off-session: PlayerState send refused");

    Check(WireSize(static_cast<MsgType>(16)) == 0,
        "no wire size exists for the removed RoomOwnership id");
    Check(WireSize(MsgType::HorseState) == HorseStateWireSize(),
        "wire id 9 is HorseState");
    Check(WireSize(static_cast<MsgType>(10)) == 0,
        "wire id 10 is gone (was EnemyEvent)");
    Check(WireSize(static_cast<MsgType>(11)) == 0,
        "wire id 11 is gone (was CombatIntent, then TimeSync)");
}

}  // namespace

int main() {
    std::printf("dusk_net_selftest: M0.5 network layer (ENet %d->%d->%d)\n", ENET_VERSION_MAJOR,
        ENET_VERSION_MINOR, ENET_VERSION_PATCH);

    // Same ENet init path the game uses (src/dusk/net/module.cpp).
    if (!dusk::net::initialize()) {
        std::fprintf(stderr, "enet_initialize failed\n");
        return 1;
    }

    RunProtocolChecks();
    RunDeterminismChecks();
    RunValidationChecks();
    RunRingPolicyChecks();
    RunRingOverflowChecks();
    RunClockChecks();
    RunGenerationGuardCheck();
    RunDuplicateJoinCheck();
    RunJoinAcceptValidationCheck();
    RunOversizedMetricCheck();
    RunHandshakeDemo();
    RunGameMessageDemo();
    RunV9WireRegression();
    RunM4WorldStageCheck();
    RunM46SessionRestartCheck();
    RunSessionStartGuardCheck();
    RunConnectingTimeoutCheck();

    dusk::net::shutdown();

    if (g_failures == 0) {
        std::printf("PASS: all checks succeeded\n");
        return 0;
    }
    std::printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
