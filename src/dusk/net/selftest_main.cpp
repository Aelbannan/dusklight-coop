/**
 * \file selftest_main.cpp
 * M0.5 LAN handshake demo + protocol/cadence/robustness self-test.
 *
 * Run (build dir):  ./dusk_net_selftest
 * Exits 0 on success, 1 on any failure.
 *
 * Covers, in one process over loopback (127.0.0.1):
 *   - wire round-trip (serialize -> deserialize -> re-serialize) for all 15
 *     message types in 00-network.md §5, plus malformed-input rejection;
 *   - wire determinism: identical logical messages serialize to identical
 *     bytes (zero-init payload unions — deepseek M3);
 *   - semantic validation: jointCount > kMaxJoints rejected at parse
 *     (deepseek M5), forged JoinAccept (bad assignedPlayerId / roster)
 *     rejected by the client session;
 *   - ring policies: reliable ring overflow is an explicit failure (Send
 *     returns false <=> counter bumps, no silent drops), snapshot rings
 *     replace-newest under pressure (glm MAJOR-1/2);
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
 *   - M2 relay policy: CombatIntent/EnemySnapshot/CombatResult/EnemyEvent do
 *     NOT ride the PlayerState star-relay — a client's CombatIntent is
 *     consumed by the sim owner and never echoed to other clients; the
 *     owner's EnemySnapshot/CombatResult simulcasts reach every client;
 *     EnemyEvent is room-scoped like EnemySnapshot (M4.5 MAJOR 2: a died/
 *     room-clear event reaches only peers in the SENDER's room — a cross-stage
 *     peer with a coincident (roomNo<<8)|setID must not mis-kill local
 *     enemies or grant wrong switches); a buggy client's
 *     EnemySnapshot/CombatResult is not echoed;
 *   - M3 time/weather contract: v5 absolute-phase wire round-trips (TimeSync
 *     f32 time + day + rate + flags; TimeEvent; WeatherChange mode + thunder
 *     + intensity + colpat); host->all broadcasts of TimeSync/TimeEvent/
 *     WeatherChange; a buggy client's time/weather messages are consumed but
 *     never relayed; JoinAccept/WorldInit carry the host's clock+sky so a
 *     mid-game joiner starts with the host's time of day and weather;
 *   - M3.5 time/weather fix pass: the TimeSync cadence gate is a REAL 1 Hz
 *     clock (deepseek MAJOR 1 — a 250 ms window emits <= 2 TimeSyncs, not the
 *     ~15 the old 60 Hz NetClock produced, and the immediate stage/rate-
 *     change sends still fire); the table-driven DeriveWeather decision
 *     (04 §4.3, incl. the thunder-with-no-rain case, deepseek M1); the
 *     defer-to-wire thunder policy (NextThunderMode); and the pond advance
 *     table with the vanilla 1x fallback (deepseek M3). These test the
 *     game-free core of coop_time.cpp (coop_time_logic.h) that the selftest
 *     can link without the game;
 *   - clean shutdown: every transport thread joined, every ENet host
 *     destroyed (leak-free exit).
 *
 * The net module logs through aurora::Module; this standalone binary provides
 * the aurora logging globals (mirroring extern/aurora/lib/logging.cpp) so it
 * links without pulling the whole aurora runtime. ENet init/deinit go through
 * the same dusk::net::initialize()/shutdown() the game uses.
 */

#include "dusk/coop/coop_entity_logic.h"
#include "dusk/coop/coop_time_logic.h"
#include "dusk/net/clock.h"
#include "dusk/net/discovery.h"
#include "dusk/net/module.h"
#include "dusk/net/protocol.h"
#include "dusk/net/session.h"

#include <aurora/aurora.h>
#include <aurora/lib/logging.hpp>
#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
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
        p.time.time = 123.5f;
        p.time.day = 7;
        p.time.rate = kTimeRateFast;
        p.time.flags = kTimeFlagDarkworld;
        p.weather.mode = static_cast<u8>(WeatherMode::RainHeavy);
        p.weather.thunder = 1;
        p.weather.intensity = 240;
        p.weather.colpat = 2;
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
        p.time.time = 205.75f;
        p.time.day = 2;
        p.time.rate = kTimeRateNormal;
        p.weather.mode = static_cast<u8>(WeatherMode::Cloudy);
        p.weather.intensity = 0;
        p.weather.colpat = 1;
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
        break;
    }
    case MsgType::EnemySnapshot: {
        auto& p = m.payload.enemySnapshot;
        p.enemyId = 77;
        p.type = 0x2041;
        p.hp = 12;
        p.maxHp = 30;
        p.aggro = 6;
        p.flags = 0xAA;
        p.angle = -12345;
        p.anim = 0x01020304;
        p.pos = Vec3f{10.0f, 20.0f, 30.0f};
        p.speed = Vec3f{-1.0f, 0.0f, 2.0f};
        p.semantics = 1;
        p.reserved[0] = 0xDE;
        p.reserved[1] = 0xAD;
        p.reserved[2] = 0xBE;
        break;
    }
    case MsgType::EnemyEvent: {
        auto& p = m.payload.enemyEvent;
        p.enemyId = 88;
        p.data = 99;
        p.eventId = static_cast<u8>(EnemyEventId::Died);
        p.flags = 0x0F;
        p.flagMask = 0x2A;
        p.reserved[0] = 0x11;
        p.reserved[1] = 0x22;
        p.reserved[2] = 0x33;
        break;
    }
    case MsgType::CombatIntent: {
        auto& p = m.payload.combatIntent;
        p.attackerId = 1;
        p.powerType = 3;
        p.hitType = 12;
        p.targetPlayerId = kInvalidPlayerId;
        p.targetEnemyId = 77;
        p.atp = 3;
        p.computedPower = 30;
        p.seq = 12345;
        p.atType = 0x00000006;  // NORMAL_SWORD | HORSE
        p.hitPos = Vec3f{0.5f, 1.5f, 2.5f};
        p.attackerPos = Vec3f{512.0f, 0.0f, -256.0f};
        break;
    }
    case MsgType::CombatResult: {
        auto& p = m.payload.combatResult;
        p.targetEnemyId = 77;
        p.damage = 4;
        p.newHp = 26;
        p.outcome = static_cast<u8>(CombatOutcome::Hit);
        p.attackerId = 1;
        p.seq = 12345;
        break;
    }
    case MsgType::TimeSync: {
        auto& p = m.payload.timeSync;
        p.time = 123.25f;
        p.day = 3;
        p.rate = kTimeRateNormal;
        p.flags = 0;
        break;
    }
    case MsgType::TimeEvent: {
        auto& p = m.payload.timeEvent;
        p.eventId = static_cast<u8>(TimeEventId::Dawn);
        p.time = 90.0f;
        p.day = 2;
        break;
    }
    case MsgType::WeatherChange: {
        auto& p = m.payload.weatherChange;
        p.mode = static_cast<u8>(WeatherMode::ThunderHeavy);
        p.thunder = 1;
        p.intensity = 250;
        p.colpat = 2;
        break;
    }
    case MsgType::RoomOwnership: {
        auto& p = m.payload.roomOwnership;
        std::strncpy(p.stage, "F_SP108", sizeof(p.stage) - 1);
        p.room = 3;
        p.owner = 2;
        p.reserved[0] = 0x12;
        p.reserved[1] = 0x34;
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
    std::printf("protocol: round-trip all 16 message types\n");
    for (u16 t = static_cast<u16>(MsgType::JoinRequest); t <= static_cast<u16>(MsgType::RoomOwnership);
         ++t)
    {
        const auto type = static_cast<MsgType>(t);
        Check(RoundTrip(type), WireSize(type) > 0 ? "round-trip ok" : "round-trip ok (size 0)");
    }
    Check(WireSize(MsgType::JoinAccept) == 1 + 3 + 20 + 8 + 6 + kMaxLocalPlayers * 36,
        "JoinAccept wire size");
    Check(WireSize(MsgType::WorldInit) == 20 + 8 + 6 + kMaxLocalPlayers * 36,
        "WorldInit wire size (stage + time + weather + roster)");
    Check(WireSize(MsgType::TimeSync) == 8 && WireSize(MsgType::TimeEvent) == 8 &&
              WireSize(MsgType::WeatherChange) == 6,
        "time/weather message sizes (absolute-phase contract)");
    Check(WireSize(MsgType::RoomOwnership) == kMaxStageNameLength + 1 + 1 + 2 &&
              ChannelFor(MsgType::RoomOwnership) == kChannelReliable,
        "RoomOwnership wire size + reliable channel (M4)");
    Check(WireSize(MsgType::PlayerState) == PlayerStateWireSize(),
        "PlayerState wire size");
    Check(ChannelFor(MsgType::PlayerState) == kChannelUnreliable &&
              ChannelFor(MsgType::JoinRequest) == kChannelReliable &&
              ChannelFor(MsgType::TimeSync) == kChannelUnreliable &&
              ChannelFor(MsgType::TimeEvent) == kChannelReliable &&
              ChannelFor(MsgType::WeatherChange) == kChannelReliable,
        "channel mapping (snapshots unreliable, control reliable; TimeSync 1 Hz unreliable, events reliable)");

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
    // M4.5 (review MINOR 5): sweep ALL 16 types — the original bound stopped
    // at WeatherChange (15) and skipped RoomOwnership (16, added in M4).
    for (u16 t = static_cast<u16>(MsgType::JoinRequest); t <= static_cast<u16>(MsgType::RoomOwnership);
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
    // the socket thread's drain, so the 128-slot ring fills and replaces).
    u64 snapOk = 0;
    for (int i = 0; i < 20000; ++i) {
        if (cliT->Send(0, kChannelUnreliable, msg, sizeof(msg))) {
            ++snapOk;
        }
    }
    Check(snapOk == 20000, "snapshot Send never fails (replace-newest)");
    // (The HOST-side replace counter is not asserted: ENet drops unreliable
    // packets at the sender once its queue exceeds the packet threshold, so
    // only the freshest reach the host and the 128-slot inbox may never fill.)
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
    host->SetGameMessageHandler([&](MsgType type, const PayloadUnion& p) {
        if (type == MsgType::PlayerState) {
            ++hostStates;
            hostStatePid = p.playerState.playerId;
        } else if (type == MsgType::PlayerEvent) {
            ++hostEvents;
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

/// M2 star-relay policy: enemy/combat traffic must NOT ride the
/// PlayerState/PlayerEvent star-relay path. A client's CombatIntent reaches
/// the sim owner (the host in v1) and is consumed there — it is never echoed
/// to the other client; the host's EnemySnapshot/CombatResult/EnemyEvent
/// simulcasts (SendGameMessage -> SendToAll) reach every client; a buggy
/// client's EnemySnapshot/CombatResult is consumed but not echoed.
void RunM2RelayPolicyCheck() {
    std::printf("m2: enemy/combat relay policy (star seam)\n");
    Demo demo;

    auto host = std::make_unique<Session>();
    SessionConfig hostCfg;
    hostCfg.port = 0;
    hostCfg.name = "M2 Host";
    hostCfg.maxPlayers = 3;
    Check(host->StartHost(hostCfg), "m2 host starts (Listening)");
    demo.live.push_back(host.get());
    const u16 port = host->boundPort();

    auto a = std::make_unique<Session>();
    SessionConfig aCfg;
    aCfg.joinHost = "127.0.0.1";
    aCfg.port = port;
    aCfg.name = "M2 Attacker";
    aCfg.version = kProtocolVersion;
    Check(a->StartClient(aCfg), "m2 attacker starts");
    demo.live.push_back(a.get());
    Check(demo.WaitFor([&] { return a->state() == SessionState::Joined; }, 10000),
        "m2 attacker joined");

    auto b = std::make_unique<Session>();
    SessionConfig bCfg;
    bCfg.joinHost = "127.0.0.1";
    bCfg.port = port;
    bCfg.name = "M2 Observer";
    bCfg.version = kProtocolVersion;
    Check(b->StartClient(bCfg), "m2 observer starts");
    demo.live.push_back(b.get());
    Check(demo.WaitFor([&] { return b->state() == SessionState::Joined; }, 10000),
        "m2 observer joined");
    Check(demo.WaitFor([&] { return PresentCountOf(*host) == 3; }, 10000),
        "m2 host roster has 3 players");

    int hostIntents = 0;
    int aIntents = 0;
    int bIntents = 0;
    int hostSnapshots = 0;
    int aSnapshots = 0;
    int bSnapshots = 0;
    int hostResults = 0;
    int aResults = 0;
    int bResults = 0;
    int aEvents = 0;
    int bEvents = 0;
    host->SetGameMessageHandler([&](MsgType type, const PayloadUnion& p) {
        switch (type) {
        case MsgType::CombatIntent:
            ++hostIntents;
            break;
        case MsgType::EnemySnapshot:
            ++hostSnapshots;
            break;
        case MsgType::CombatResult:
            ++hostResults;
            break;
        default:
            break;
        }
    });
    a->SetGameMessageHandler([&](MsgType type, const PayloadUnion& p) {
        switch (type) {
        case MsgType::CombatIntent:
            ++aIntents;
            break;
        case MsgType::EnemySnapshot:
            ++aSnapshots;
            break;
        case MsgType::CombatResult:
            ++aResults;
            break;
        case MsgType::EnemyEvent:
            ++aEvents;
            break;
        default:
            break;
        }
    });
    b->SetGameMessageHandler([&](MsgType type, const PayloadUnion& p) {
        switch (type) {
        case MsgType::CombatIntent:
            ++bIntents;
            break;
        case MsgType::EnemySnapshot:
            ++bSnapshots;
            break;
        case MsgType::CombatResult:
            ++bResults;
            break;
        case MsgType::EnemyEvent:
            ++bEvents;
            break;
        default:
            break;
        }
    });

    // 1) CombatIntent: client A -> sim owner (host). The host consumes it for
    //    validation; it must NOT be relayed to observer B or echoed back to A.
    PayloadUnion intent = {};
    intent.combatIntent.attackerId = a->selfId();
    intent.combatIntent.targetEnemyId = 77;
    intent.combatIntent.atp = 3;
    intent.combatIntent.powerType = 1;
    intent.combatIntent.seq = 1;
    Check(a->SendGameMessage(MsgType::CombatIntent, intent), "A sends CombatIntent");
    Check(demo.WaitFor([&] { return hostIntents >= 1; }, 10000),
        "sim owner consumed A's CombatIntent");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Check(aIntents == 0 && bIntents == 0,
        "CombatIntent is NOT relayed to other clients");

    // 2) EnemySnapshot: owner (host) simulcast reaches both clients.
    PayloadUnion snap = {};
    snap.enemySnapshot.enemyId = 77;
    snap.enemySnapshot.type = 0x01AF;
    snap.enemySnapshot.hp = 42;
    snap.enemySnapshot.maxHp = 100;
    Check(host->SendGameMessage(MsgType::EnemySnapshot, snap), "host sends EnemySnapshot");
    Check(demo.WaitFor([&] { return aSnapshots >= 1 && bSnapshots >= 1; }, 10000),
        "clients received the host's EnemySnapshot");

    // 3) A buggy client's EnemySnapshot is consumed but NOT echoed to B.
    PayloadUnion clientSnap = {};
    clientSnap.enemySnapshot.enemyId = 78;
    clientSnap.enemySnapshot.type = 0x01AF;
    Check(a->SendGameMessage(MsgType::EnemySnapshot, clientSnap), "A sends EnemySnapshot");
    Check(demo.WaitFor([&] { return hostSnapshots >= 1; }, 10000),
        "sim owner consumed A's EnemySnapshot");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Check(bSnapshots == 1, "A's EnemySnapshot is NOT relayed to B");

    // 4) CombatResult: owner simulcast reaches both clients.
    PayloadUnion result = {};
    result.combatResult.targetEnemyId = 77;
    result.combatResult.damage = 4;
    result.combatResult.newHp = 38;
    result.combatResult.outcome = static_cast<u8>(CombatOutcome::Hit);
    result.combatResult.attackerId = a->selfId();
    result.combatResult.seq = 1;
    Check(host->SendGameMessage(MsgType::CombatResult, result), "host sends CombatResult");
    Check(demo.WaitFor([&] { return aResults >= 1 && bResults >= 1; }, 10000),
        "CombatResult simulcast reached both clients");

    // 5) EnemyEvent(died) from the host: no room was established in this
    //    test, so the room-scoped fan-out opens (unknown-room rule — same as
    //    the sender gate) and reaches both clients. (M4.5 MAJOR 2: with rooms
    //    established, died/room-clear events reach only the room's players.)
    PayloadUnion ev = {};
    ev.enemyEvent.enemyId = 77;
    ev.enemyEvent.eventId = static_cast<u8>(EnemyEventId::Died);
    ev.enemyEvent.data = 0x1E;  // drop table id
    Check(host->SendGameMessage(MsgType::EnemyEvent, ev), "host sends EnemyEvent(died)");
    Check(demo.WaitFor([&] { return aEvents >= 1 && bEvents >= 1; }, 10000),
        "EnemyEvent reach both clients with no room established (open gate)");

    // 6) A buggy client's CombatResult is consumed but NOT echoed to B.
    PayloadUnion rogueResult = {};
    rogueResult.combatResult.targetEnemyId = 99;
    Check(a->SendGameMessage(MsgType::CombatResult, rogueResult), "A sends CombatResult");
    Check(demo.WaitFor([&] { return hostResults >= 1; }, 10000),
        "sim owner consumed A's CombatResult");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Check(bResults == 1, "A's CombatResult is NOT relayed to B");

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
/// M3 time/weather (v5 absolute-phase contract): field-exact wire round-trips
/// (04 §4); the host broadcasts TimeSync/TimeEvent/WeatherChange host->all; a
/// client's (buggy/forged) time/weather messages are consumed on the host but
/// never relayed; JoinAccept/WorldInit carry the host's clock+sky so a
/// mid-game joiner starts with the host's time of day and weather (task 6).
void RunM3TimeWeatherCheck() {
    std::printf("m3: time/weather sync contract\n");
    Demo demo;

    // 1) Field-exact wire round-trips (absolute-phase contract, 04 §4).
    {
        Message ts = MakeMessage(MsgType::TimeSync);
        u8 buf[kMaxMessageSize];
        ByteWriter w(buf, sizeof(buf));
        Check(SerializeMessage(ts, w), "TimeSync serializes");
        ByteReader r(buf, w.size());
        Message parsed;
        Check(DeserializeMessage(r, parsed), "TimeSync deserializes");
        Check(parsed.payload.timeSync.time == ts.payload.timeSync.time &&
                  parsed.payload.timeSync.day == ts.payload.timeSync.day &&
                  parsed.payload.timeSync.rate == ts.payload.timeSync.rate &&
                  parsed.payload.timeSync.flags == ts.payload.timeSync.flags,
            "TimeSync fields survive the round-trip");

        Message te = MakeMessage(MsgType::TimeEvent);
        u8 buf2[kMaxMessageSize];
        ByteWriter w2(buf2, sizeof(buf2));
        Check(SerializeMessage(te, w2), "TimeEvent serializes");
        ByteReader r2(buf2, w2.size());
        Message parsed2;
        Check(DeserializeMessage(r2, parsed2), "TimeEvent deserializes");
        Check(parsed2.payload.timeEvent.eventId == static_cast<u8>(TimeEventId::Dawn) &&
                  parsed2.payload.timeEvent.time == 90.0f &&
                  parsed2.payload.timeEvent.day == 2,
            "TimeEvent fields survive the round-trip");

        Message wc = MakeMessage(MsgType::WeatherChange);
        u8 buf3[kMaxMessageSize];
        ByteWriter w3(buf3, sizeof(buf3));
        Check(SerializeMessage(wc, w3), "WeatherChange serializes");
        ByteReader r3(buf3, w3.size());
        Message parsed3;
        Check(DeserializeMessage(r3, parsed3), "WeatherChange deserializes");
        Check(parsed3.payload.weatherChange.mode == static_cast<u8>(WeatherMode::ThunderHeavy) &&
                  parsed3.payload.weatherChange.thunder == 1 &&
                  parsed3.payload.weatherChange.intensity == 250 &&
                  parsed3.payload.weatherChange.colpat == 2,
            "WeatherChange fields survive the round-trip");
    }

    auto host = std::make_unique<Session>();
    SessionConfig hostCfg;
    hostCfg.port = 0;
    hostCfg.name = "M3 Host";
    hostCfg.maxPlayers = 3;
    Check(host->StartHost(hostCfg), "m3 host starts (Listening)");
    demo.live.push_back(host.get());
    const u16 port = host->boundPort();

    // The host has a clock and a sky BEFORE joiners arrive (task 6 — the
    // coop publisher updates this every frame in-game; here the test seeds
    // the session world info directly).
    TimeStateInfo hostTime = {};
    hostTime.time = 331.25f;  // 22:05 — near dusk
    hostTime.day = 5;
    hostTime.rate = kTimeRateNormal;
    WeatherStateInfo hostWeather = {};
    hostWeather.mode = static_cast<u8>(WeatherMode::RainLight);
    hostWeather.intensity = 40;
    hostWeather.colpat = 1;
    host->setWorldTime(hostTime);
    host->setWorldWeather(hostWeather);

    auto a = std::make_unique<Session>();
    SessionConfig aCfg;
    aCfg.joinHost = "127.0.0.1";
    aCfg.port = port;
    aCfg.name = "M3 A";
    aCfg.version = kProtocolVersion;
    Check(a->StartClient(aCfg), "m3 client A starts");
    demo.live.push_back(a.get());
    Check(demo.WaitFor([&] { return a->state() == SessionState::Joined; }, 10000), "A joined");
    Check(a->worldTime().time == hostTime.time && a->worldTime().day == hostTime.day &&
              a->worldTime().rate == hostTime.rate,
        "JoinAccept carries the host's clock to the joiner");
    Check(a->worldWeather().mode == hostWeather.mode &&
              a->worldWeather().intensity == hostWeather.intensity &&
              a->worldWeather().colpat == hostWeather.colpat,
        "JoinAccept carries the host's sky to the joiner");

    auto b = std::make_unique<Session>();
    SessionConfig bCfg;
    bCfg.joinHost = "127.0.0.1";
    bCfg.port = port;
    bCfg.name = "M3 B";
    bCfg.version = kProtocolVersion;
    Check(b->StartClient(bCfg), "m3 client B starts");
    demo.live.push_back(b.get());
    Check(demo.WaitFor([&] { return b->state() == SessionState::Joined; }, 10000), "B joined");
    Check(demo.WaitFor([&] { return PresentCountOf(*host) == 3; }, 10000),
        "host roster has 3 players");
    Check(demo.WaitFor([&] { return a->roster()[b->selfId()].present; }, 10000),
        "A learned about B via the WorldInit re-broadcast");

    int hostSyncs = 0, aSyncs = 0, bSyncs = 0;
    int hostEvents = 0, aEvents = 0;
    int hostWeatherCount = 0, aWeatherCount = 0, bWeatherCount = 0;
    host->SetGameMessageHandler([&](MsgType type, const PayloadUnion& p) {
        switch (type) {
        case MsgType::TimeSync:
            ++hostSyncs;
            break;
        case MsgType::TimeEvent:
            ++hostEvents;
            break;
        case MsgType::WeatherChange:
            ++hostWeatherCount;
            break;
        default:
            break;
        }
    });
    a->SetGameMessageHandler([&](MsgType type, const PayloadUnion& p) {
        switch (type) {
        case MsgType::TimeSync:
            ++aSyncs;
            break;
        case MsgType::TimeEvent:
            ++aEvents;
            break;
        case MsgType::WeatherChange:
            ++aWeatherCount;
            break;
        default:
            break;
        }
    });
    b->SetGameMessageHandler([&](MsgType type, const PayloadUnion& p) {
        switch (type) {
        case MsgType::TimeSync:
            ++bSyncs;
            break;
        case MsgType::WeatherChange:
            ++bWeatherCount;
            break;
        default:
            break;
        }
    });

    // Host -> all broadcasts (the host owns the clock and the sky).
    PayloadUnion sync = {};
    sync.timeSync.time = 332.0f;
    sync.timeSync.day = 5;
    sync.timeSync.rate = kTimeRateNormal;
    Check(host->SendGameMessage(MsgType::TimeSync, sync), "host sends TimeSync");
    Check(demo.WaitFor([&] { return aSyncs >= 1 && bSyncs >= 1; }, 10000),
        "TimeSync reached both clients");

    PayloadUnion ev = {};
    ev.timeEvent.eventId = static_cast<u8>(TimeEventId::Dusk);
    ev.timeEvent.time = 332.0f;
    ev.timeEvent.day = 5;
    Check(host->SendGameMessage(MsgType::TimeEvent, ev), "host sends TimeEvent(Dusk)");
    Check(demo.WaitFor([&] { return aEvents >= 1; }, 10000), "TimeEvent reached client A");

    PayloadUnion wc = {};
    wc.weatherChange.mode = static_cast<u8>(WeatherMode::RainHeavy);
    wc.weatherChange.thunder = 0;
    wc.weatherChange.intensity = 250;
    wc.weatherChange.colpat = 2;
    Check(host->SendGameMessage(MsgType::WeatherChange, wc), "host sends WeatherChange");
    Check(demo.WaitFor([&] { return aWeatherCount >= 1 && bWeatherCount >= 1; }, 10000),
        "WeatherChange reached both clients");

    // Clients NEVER relay time/weather: a buggy client's messages are
    // consumed by the host (handler) but not echoed to the other client.
    PayloadUnion rogueSync = {};
    rogueSync.timeSync.time = 42.0f;
    Check(a->SendGameMessage(MsgType::TimeSync, rogueSync), "A sends a rogue TimeSync");
    Check(demo.WaitFor([&] { return hostSyncs >= 1; }, 10000),
        "host consumed A's rogue TimeSync");
    PayloadUnion rogueWc = {};
    rogueWc.weatherChange.mode = static_cast<u8>(WeatherMode::Clear);
    Check(a->SendGameMessage(MsgType::WeatherChange, rogueWc), "A sends a rogue WeatherChange");
    Check(demo.WaitFor([&] { return hostWeatherCount >= 1; }, 10000),
        "host consumed A's rogue WeatherChange");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Check(bSyncs == 1 && bWeatherCount == 1,
        "A's rogue TimeSync/WeatherChange are NOT relayed to B");

    for (Session* s : demo.live) {
        s->Stop();
    }
    demo.live.clear();
    host->Stop();
}

// ---------------------------------------------------------------------------
// M3.5 fix pass — TimeSync 1 Hz cadence + the pure time/weather decisions
// (deepseek MAJOR 1 / MINORs 1, 3; glm MINOR 1)
// ---------------------------------------------------------------------------

/// MAJOR 1 (deepseek): TimeSync was published at ~60 Hz because NetClock
/// defaults to 60 Hz and PublishHostState used it as a "one second" gate.
/// The fix is NetClock::AtRate(1) + the TimeSyncDue predicate; the checks
/// below drive the REAL gate (NetClock::AtRate(1)) and the REAL decision
/// functions (coop_time_logic.h — the game-free core of coop_time.cpp that
/// this selftest can link without the game) over simulated 60 Hz sim frames.
void RunM35TimeWeatherFixCheck() {
    std::printf("m3.5: time/weather fix pass\n");
    using namespace dusk::coop::timeweather;

    // -- 1) The 1 Hz cadence gate (deepseek MAJOR 1) ----------------------
    {
        // A 250 ms window of 60 Hz sim frames: the 1 Hz gate fires zero
        // ticks. The old 60 Hz clock fired ~15 — asserted below as the
        // regression bound this check exists to catch.
        NetClock sync = NetClock::AtRate(1);
        int ticks250ms = 0;
        for (u64 f = 0; f < 15; ++f) {  // 15 frames @ 60 Hz = 250 ms
            if (sync.Tick(f * NetClock::kIntervalUs)) {
                ++ticks250ms;
            }
        }
        Check(ticks250ms <= 2, "TimeSync cadence: <= 2 ticks in a 250 ms window");

        // The same 250 ms window on the DEFAULT 60 Hz clock — the old bug:
        // proves the check would have caught MAJOR 1.
        NetClock fast = NetClock();
        int fastTicks250ms = 0;
        for (u64 f = 0; f < 15; ++f) {
            if (fast.Tick(f * NetClock::kIntervalUs)) {
                ++fastTicks250ms;
            }
        }
        Check(fastTicks250ms > 10, "regression guard: the old 60 Hz clock fires ~15x in 250 ms");

        // Full seconds fire exactly once each (2 s at 60 Hz sim -> frames 60
        // and 120; 120 is past the window, so 1 tick in 0..119 plus the 250 ms
        // prefix already consumed). Continue the same clock: it must tick once
        // per second and never burst.
        int ticks1s = 0, ticks2s = 0;
        for (u64 f = 15; f < 60; ++f) {
            ticks1s += sync.Tick(f * NetClock::kIntervalUs) ? 1 : 0;
        }
        for (u64 f = 60; f < 120; ++f) {
            ticks2s += sync.Tick(f * NetClock::kIntervalUs) ? 1 : 0;
        }
        Check(ticks1s == 0 && ticks2s == 1,
            "TimeSync cadence: one tick at the 1 s boundary, none between");
        Check(!sync.Tick(119 * NetClock::kIntervalUs + 1),
            "TimeSync cadence: quiet just after the second boundary");

        // Catch-up: a multi-second stall ticks ONCE (no burst).
        NetClock gap = NetClock::AtRate(1);
        Check(gap.Tick(0) == false && gap.Tick(4 * 1000000) == true && gap.Frame() == 1,
            "TimeSync cadence: a 4 s stall catches up in a single tick");
    }

    // -- 2) The publish decision: 1 Hz cadence + immediate sends -----------
    {
        // Simulate 2 s of host publishing at 60 Hz with one stage change and
        // one rate change injected. Cadence contributes the 1 s / 2 s sends;
        // the stage/rate changes must still fire immediately (04 §4.1).
        NetClock sync = NetClock::AtRate(1);
        int sends = 0;
        u8 rate = kTimeRateNormal, lastRate = kTimeRateNormal;
        bool stageChanged = false;
        for (u64 f = 0; f < 121; ++f) {  // 2 s + one frame: both boundaries
            if (f == 30) {
                stageChanged = true;  // stage load re-assert (04 §5.1)
            }
            if (f == 45) {
                rate = kTimeRateFast;  // wolf-howl skip (04 §5.4)
            }
            if (TimeSyncDue(SyncDueInput{sync.Tick(f * NetClock::kIntervalUs),
                                          stageChanged, rate, lastRate}))
            {
                ++sends;
                lastRate = rate;
            }
            stageChanged = false;
        }
        // 2 cadence sends (f=60, f=120) + 1 stage + 1 rate.
        Check(sends == 4, "TimeSync sends: 2 cadence + immediate stage + immediate rate");

        // The immediate paths alone (no cadence ticks in a 250 ms window).
        NetClock quiet = NetClock::AtRate(1);
        int immediate = 0;
        u8 r2 = kTimeRateNormal;
        for (u64 f = 0; f < 15; ++f) {
            const bool stage = (f == 5);
            const u8 r = (f == 10) ? kTimeRateFrozen : r2;
            if (TimeSyncDue(SyncDueInput{quiet.Tick(f * NetClock::kIntervalUs),
                                          stage, r, r2}))
            {
                ++immediate;
                r2 = r;
            }
        }
        Check(immediate == 2, "immediate stage/rate sends still fire inside the 250 ms window");
    }

    // -- 3) DeriveWeather decision table (04 §4.3; deepseek M1) -----------
    {
        // (raincnt, snow, thunder, colpat, diceStage, diceMode) -> mode. The
        // thunder-with-no-rain cases document that the MODE collapses to
        // Cloudy/Clear while the wire still carries thunder=1 (NextThunderMode
        // defers to that carried bit).
        struct DeriveCase {
            int raincnt, snow;
            u8 thunder, colpat;
            bool dice;
            u8 diceMode;
            u8 expectMode;
            u16 expectIntensity;
        };
        const DeriveCase kCases[] = {
            // clear / cloudy skies
            {0, 0, 0, 0, false, 0, static_cast<u8>(WeatherMode::Clear), 0},
            {0, 0, 0, 1, false, 0, static_cast<u8>(WeatherMode::Cloudy), 0},
            // thunder with no rain (deepseek M1): mode collapses, wire keeps
            // thunder=1 + the palette colpat
            {0, 0, 1, 1, false, 0, static_cast<u8>(WeatherMode::Cloudy), 0},
            {0, 0, 1, 0, false, 0, static_cast<u8>(WeatherMode::Clear), 0},
            // rain branches (04 §4.3 table)
            {40, 0, 0, 1, false, 0, static_cast<u8>(WeatherMode::RainLight), 40},
            {250, 0, 0, 2, false, 0, static_cast<u8>(WeatherMode::RainHeavy), 250},
            {10, 0, 1, 1, false, 0, static_cast<u8>(WeatherMode::ThunderLight), 10},
            {250, 0, 1, 2, false, 0, static_cast<u8>(WeatherMode::ThunderHeavy), 250},
            // colpat 0 + rain in the air = teardown drain
            {5, 0, 0, 0, false, 0, static_cast<u8>(WeatherMode::Clear), 5},
            // snow stages
            {0, 300, 0, 1, false, 0, static_cast<u8>(WeatherMode::Snow), 300},
            // dice stages map the machine directly (F_SP108/121/127)
            {0, 0, 0, 0, true, 0, static_cast<u8>(WeatherMode::Clear), 0},
            {0, 0, 0, 0, true, 1, static_cast<u8>(WeatherMode::Cloudy), 0},
            {40, 0, 0, 0, true, 2, static_cast<u8>(WeatherMode::RainLight), 40},
            {0, 0, 0, 0, true, 4, static_cast<u8>(WeatherMode::ThunderLight), 0},
            {250, 0, 0, 0, true, 5, static_cast<u8>(WeatherMode::ThunderHeavy), 250},
            {0, 0, 1, 0, true, 6, static_cast<u8>(WeatherMode::Clear), 0},  // UNK6 -> Clear
        };
        bool ok = true;
        bool thunderCarried = true;
        for (const auto& c : kCases) {
            SkyDeriveInput in;
            in.raincnt = c.raincnt;
            in.snowCount = c.snow;
            in.thunder = c.thunder;
            in.colpat = c.colpat;
            in.diceStage = c.dice;
            in.diceMode = c.diceMode;
            const DeriveResult r = DeriveWeatherFrom(in);
            ok = ok && r.mode == c.expectMode && r.intensity == c.expectIntensity;
            thunderCarried = thunderCarried && (r.thunder == (c.thunder != 0 ? 1 : 0));
        }
        Check(ok, "DeriveWeather mode/intensity table (incl. thunder-with-no-rain)");
        Check(thunderCarried,
            "DeriveWeather always carries the live thunder bit on the wire");
    }

    // -- 4) Defer-to-wire thunder policy (deepseek M1) ---------------------
    {
        // (wireMode, wireThunder, current) -> next mMode.
        struct ThunderCase {
            u8 mode, wireThunder, current, expect;
        };
        const ThunderCase kCases[] = {
            // Cloudy: only clear when the wire says 0 (the fix — a held
            // synced thunder with a Cloudy derivation must keep flashing)
            {static_cast<u8>(WeatherMode::Cloudy), 1, 1, 1},
            {static_cast<u8>(WeatherMode::Cloudy), 1, 0, 1},
            {static_cast<u8>(WeatherMode::Cloudy), 0, 1, 0},
            {static_cast<u8>(WeatherMode::Cloudy), 0, 2, 0},
            // Clear: clears a 1 only when the wire says 0; kytag00's 2
            // survives
            {static_cast<u8>(WeatherMode::Clear), 1, 1, 1},
            {static_cast<u8>(WeatherMode::Clear), 0, 1, 0},
            {static_cast<u8>(WeatherMode::Clear), 0, 2, 2},
            // thunder modes force 1
            {static_cast<u8>(WeatherMode::ThunderLight), 1, 0, 1},
            {static_cast<u8>(WeatherMode::ThunderHeavy), 1, 2, 1},
            // rain/snow never touch it (kytag00 area state survives)
            {static_cast<u8>(WeatherMode::RainLight), 0, 2, 2},
            {static_cast<u8>(WeatherMode::Snow), 1, 2, 2},
        };
        bool ok = true;
        for (const auto& c : kCases) {
            ok = ok && NextThunderMode(c.mode, c.wireThunder, c.current) == c.expect;
        }
        Check(ok, "ThunderPerMode defers to the wire (clears only when thunder==0)");
    }

    // -- 5) Pond advance table (deepseek M3) -------------------------------
    {
        struct RateCase {
            u8 rate;
            f32 daytime;
            bool pond;
            f32 expect;
        };
        const RateCase kCases[] = {
            {kTimeRateNormal, 123.0f, false, 0.012f},
            {kTimeRateFast, 100.0f, false, 1.0f},
            {kTimeRateFrozen, 100.0f, false, 0.0f},
            // pond windows stay exact (vanilla triple/double)
            {kTimeRatePond2x, 300.0f, true, 0.036f},
            {kTimeRatePond2x, 45.0f, true, 0.036f},
            {kTimeRatePond2x, 150.0f, true, 0.024f},
            {kTimeRatePond2x, 180.0f, true, 0.024f},
            // fallback outside the windows is vanilla 1x (the fix; was 2x)
            {kTimeRatePond2x, 100.0f, true, 0.012f},
            {kTimeRatePond2x, 240.0f, true, 0.012f},
            // rate 3 on a non-pond stage (shouldn't happen; same 1x fallback)
            {kTimeRatePond2x, 100.0f, false, 0.012f},
        };
        bool ok = true;
        for (const auto& c : kCases) {
            ok = ok && RatePerTick(c.rate, c.daytime, c.pond) == c.expect;
        }
        Check(ok, "AdvanceStep pond table (exact windows, vanilla 1x fallback)");
    }

    // -- 6) Seed-adoption decision (deepseek MINOR B) ----------------------
    {
        struct SeedCase {
            bool timeValid;
            bool timeChanged;
            bool weatherChanged;
            bool expectAdoptTime;
            bool expectAdoptWeather;
        };
        // first-join adopts the clock (no TimeSync yet, world state present)
        const SeedCase kCases[] = {
            {false, true, true, true, true},
            // a later roster-refresh with g_time.valid does NOT regress the
            // clock (an unreliable TimeSync can overtake the reliable
            // WorldInit it precedes)
            {true, true, true, false, true},
            // same-world-state re-seed with no actual change: nothing
            {false, false, false, false, false},
            // weather re-seeds unconditionally on change even when the clock
            // is already valid
            {true, false, true, false, true},
        };
        bool ok = true;
        for (const auto& c : kCases) {
            const SeedDecision d = SeedTargetsDecision(
                SeedInput{c.timeValid, c.timeChanged, c.weatherChanged});
            ok = ok && d.adoptTime == c.expectAdoptTime &&
                 d.adoptWeather == c.expectAdoptWeather;
        }
        Check(ok,
            "SeedTargetsDecision table (first-join adopts, later refresh does not, weather always)");
    }

    // -- 7) WeatherChange publish decision incl. the thunder edge ----------
    //     (glm M3.5 MINOR 1): a thunder-only transition (kytag00 area-tag
    //     arming / wether-proc case 5) must publish even when the derived
    //     mode does not move. Non-tautological: the pre-fix mode-only gate
    //     (without thunderChanged) returns false for the exact edge case.
    {
        // (modeChanged, snowDrift, stageChanged, thunderChanged) -> publish
        struct PublishCase {
            bool mode, snow, stage, thunder, expect;
        };
        const PublishCase kCases[] = {
            // thunder-only edge: the fix publishes, the old gate would not
            {false, false, false, true, true},
            // unchanged sky: nothing
            {false, false, false, false, false},
            // ordinary mode change / stage change / snow drift still publish
            {true, false, false, false, true},
            {false, false, true, false, true},
            {false, true, false, false, true},
        };
        bool ok = true;
        bool oldGateMisses = false;
        for (const auto& c : kCases) {
            ok = ok &&
                 WeatherPublishDue(WeatherPublishInput{c.mode, c.snow, c.stage, c.thunder}) ==
                     c.expect;
            // the pre-fix mode-only gate: mode || snow || stage
            if (!c.mode && !c.snow && !c.stage && c.thunder) {
                oldGateMisses = true;  // this case is exactly what the fix adds
            }
        }
        Check(ok, "WeatherPublishDue table (thunder-only edge publishes)");
        Check(oldGateMisses,
            "thunder-only case is NOT publishable by the old mode-only gate (regression guard)");
    }
}

/// slot after its ~5 s peer timeout). The generation bumps on release and
/// reassign; A's stale snapshot is dropped at Poll time and D's fresh
/// traffic flows normally.
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
        // Drain whatever remains: A's stale snapshot must have been dropped by
        // the generation guard, never delivered as if from D.
        bool sawStale = false;
        InboundPacket p;
        while (hostT->Poll(p)) {
            if (p.type == NetEventType::Data) {
                sawStale = sawStale || (p.size == 5 && std::memcmp(p.data, "STALE", 5) == 0);
            }
        }
        Check(!sawStale, "host never delivered A's stale snapshot to D");
        Check(hostT->InboundGenerationDropped() == 1,
            "stale inbound packet dropped by the generation guard");

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

/// Pure table: sticky first-in-room ownership, host-defaults-own-its-room,
/// transfer on leave/disconnect, never on arrival (no ping-pong).
void RunM4OwnershipTableCheck() {
    std::printf("m4: room ownership table (sticky first-in, host default, transfer)\n");

    // 1) First player in the room owns it; a later arrival does NOT take it
    //    over (sticky — no ping-pong).
    {
        RoomOwnershipTable t;
        std::vector<RoomKey> changed;
        Check(t.OwnerOf("F_SP108", 2) == kInvalidPlayerId, "empty room has no owner");
        t.OnPlayerEnter("F_SP108", 2, 3, /*isHost=*/false, &changed);
        Check(t.OwnerOf("F_SP108", 2) == 3, "first player owns the room");
        changed.clear();
        t.OnPlayerEnter("F_SP108", 2, 4, /*isHost=*/false, &changed);
        Check(t.OwnerOf("F_SP108", 2) == 3,
            "later arrival does not take ownership (sticky, no ping-pong)");
        Check(changed.empty(), "no ownership broadcast on a non-transferring arrival");
    }

    // 2) The world host defaults to owning its own room — even over an
    //    earlier client arrival.
    {
        RoomOwnershipTable t;
        std::vector<RoomKey> changed;
        t.OnPlayerEnter("F_SP108", 1, 2, /*isHost=*/false, &changed);
        t.OnPlayerEnter("F_SP108", 1, 3, /*isHost=*/false, &changed);
        Check(t.OwnerOf("F_SP108", 1) == 2, "client first-in owns the room");
        changed.clear();
        t.OnPlayerEnter("F_SP108", 1, 0, /*isHost=*/true, &changed);
        Check(t.OwnerOf("F_SP108", 1) == 0,
            "host entering takes ownership (host-defaults-own-its-room)");
        Check(!changed.empty(), "host arrival is an ownership change (broadcast)");
    }

    // 3) Owner leaves -> next player in the room takes over; the room stays
    //    owned. All leave -> ownerless.
    {
        RoomOwnershipTable t;
        std::vector<RoomKey> changed;
        t.OnPlayerEnter("F_SP108", 3, 1, false, &changed);
        t.OnPlayerEnter("F_SP108", 3, 2, false, &changed);
        t.OnPlayerEnter("F_SP108", 3, 5, false, &changed);
        Check(t.OwnerOf("F_SP108", 3) == 1, "first arrival owns");
        changed.clear();
        t.OnPlayerLeave("F_SP108", 3, 1, false, &changed);
        Check(t.OwnerOf("F_SP108", 3) == 2, "owner leave transfers to the next player");
        Check(!changed.empty(), "takeover is broadcast");
        changed.clear();
        t.OnPlayerLeave("F_SP108", 3, 2, false, &changed);
        Check(t.OwnerOf("F_SP108", 3) == 5, "second transfer to the remaining player");
        changed.clear();
        t.OnPlayerLeave("F_SP108", 3, 5, false, &changed);
        Check(t.OwnerOf("F_SP108", 3) == kInvalidPlayerId, "empty room becomes ownerless");
        Check(!changed.empty(), "ownerless broadcast");
    }

    // 4) Disconnect removes the player from every room and transfers.
    {
        RoomOwnershipTable t;
        std::vector<RoomKey> changed;
        t.OnPlayerEnter("F_SP108", 2, 4, false, &changed);
        t.OnPlayerEnter("F_SP108", 2, 6, false, &changed);
        t.OnPlayerEnter("F_SP104", 1, 6, false, &changed);
        Check(t.OwnerOf("F_SP108", 2) == 4 && t.OwnerOf("F_SP104", 1) == 6,
            "two rooms owned");
        changed.clear();
        t.OnPlayerDisconnect(6, false, &changed);
        Check(t.OwnerOf("F_SP108", 2) == 4, "room unaffected by the other room's owner");
        Check(t.OwnerOf("F_SP104", 1) == kInvalidPlayerId,
            "disconnected owner leaves its room ownerless");
    }

    // 5) Stage is part of the key: same room number on another stage is a
    //    separate room with a separate owner.
    {
        RoomOwnershipTable t;
        std::vector<RoomKey> changed;
        t.OnPlayerEnter("F_SP102", 1, 3, false, &changed);
        t.OnPlayerEnter("F_SP108", 1, 4, false, &changed);
        Check(t.OwnerOf("F_SP102", 1) == 3 && t.OwnerOf("F_SP108", 1) == 4,
            "room number alone does not co-own across stages");
    }

    // 6) SetOwner (client view) applies the host's authoritative assignment.
    {
        RoomOwnershipTable t;
        t.SetOwner("F_SP108", 2, 3);
        Check(t.OwnerOf("F_SP108", 2) == 3, "SetOwner assigns");
        t.SetOwner("F_SP108", 2, 7);
        Check(t.OwnerOf("F_SP108", 2) == 7, "SetOwner replaces (host authority)");
        t.SetOwner("F_SP108", 2, kInvalidPlayerId);
        Check(t.OwnerOf("F_SP108", 2) == kInvalidPlayerId, "SetOwner clears (ownerless)");
    }
}

/// Full-session: CombatIntent routes to the ROOM owner (which may be a
/// client), never to other clients and not to the host's handler unless the
/// host owns the room; EnemySnapshot from a client owner is relayed only to
/// peers in the sender's room; CombatResult from a client owner reaches
/// everyone; ownership stays sticky and transfers on leave.
void RunM4RoomRoutingCheck() {
    std::printf("m4: room-owner routing + same-room snapshot/event scoping (sessions)\n");
    Demo demo;

    auto host = std::make_unique<Session>();
    SessionConfig hostCfg;
    hostCfg.port = 0;
    hostCfg.name = "M4 Host";
    hostCfg.maxPlayers = 3;
    Check(host->StartHost(hostCfg), "m4 host starts (Listening)");
    demo.live.push_back(host.get());
    const u16 port = host->boundPort();
    // The host's own room: room 1 (host-defaults-own-its-room).
    host->setLocalRoom("F_SP108", 1);

    auto a = std::make_unique<Session>();
    SessionConfig aCfg;
    aCfg.joinHost = "127.0.0.1";
    aCfg.port = port;
    aCfg.name = "M4 Attacker";
    aCfg.version = kProtocolVersion;
    Check(a->StartClient(aCfg), "m4 attacker starts");
    demo.live.push_back(a.get());
    Check(demo.WaitFor([&] { return a->state() == SessionState::Joined; }, 10000),
        "m4 attacker joined");

    auto b = std::make_unique<Session>();
    SessionConfig bCfg;
    bCfg.joinHost = "127.0.0.1";
    bCfg.port = port;
    bCfg.name = "M4 Owner";
    bCfg.version = kProtocolVersion;
    Check(b->StartClient(bCfg), "m4 owner starts");
    demo.live.push_back(b.get());
    Check(demo.WaitFor([&] { return b->state() == SessionState::Joined; }, 10000),
        "m4 owner joined");
    Check(demo.WaitFor([&] { return PresentCountOf(*host) == 3; }, 10000),
        "m4 host roster has 3 players");

    int hostIntents = 0;
    int aIntents = 0;
    int bIntents = 0;
    int hostSnapshots = 0;
    int aSnapshots = 0;
    int bSnapshots = 0;
    int aResults = 0;
    int bResults = 0;
    int cResults = 0;
    int hostEvents = 0;
    int aEvents = 0;
    int bEvents = 0;
    host->SetGameMessageHandler([&](MsgType type, const PayloadUnion& p) {
        switch (type) {
        case MsgType::CombatIntent:
            ++hostIntents;
            break;
        case MsgType::EnemySnapshot:
            ++hostSnapshots;
            break;
        case MsgType::CombatResult:
            ++cResults;
            break;
        case MsgType::EnemyEvent:
            ++hostEvents;
            break;
        default:
            break;
        }
    });
    a->SetGameMessageHandler([&](MsgType type, const PayloadUnion& p) {
        switch (type) {
        case MsgType::CombatIntent:
            ++aIntents;
            break;
        case MsgType::EnemySnapshot:
            ++aSnapshots;
            break;
        case MsgType::CombatResult:
            ++aResults;
            break;
        case MsgType::EnemyEvent:
            ++aEvents;
            break;
        default:
            break;
        }
    });
    b->SetGameMessageHandler([&](MsgType type, const PayloadUnion& p) {
        switch (type) {
        case MsgType::CombatIntent:
            ++bIntents;
            break;
        case MsgType::EnemySnapshot:
            ++bSnapshots;
            break;
        case MsgType::CombatResult:
            ++bResults;
            break;
        case MsgType::EnemyEvent:
            ++bEvents;
            break;
        default:
            break;
        }
    });

    // -- room establishment: B enters room 2 first (owner=B), then A (sticky).
    PayloadUnion bRoom = {};
    bRoom.playerState.playerId = b->selfId();
    std::strncpy(bRoom.playerState.stage, "F_SP108", sizeof(bRoom.playerState.stage) - 1);
    bRoom.playerState.roomNo = 2;
    Check(b->SendGameMessage(MsgType::PlayerState, bRoom), "B announces room 2");
    Check(demo.WaitFor([&] { return host->roomOwner("F_SP108", 2) == b->selfId(); }, 10000),
        "B owns room 2 (first in)");
    Check(demo.WaitFor([&] { return a->roomOwner("F_SP108", 2) == b->selfId(); }, 10000),
        "ownership broadcast reached A");

    PayloadUnion aRoom = {};
    aRoom.playerState.playerId = a->selfId();
    std::strncpy(aRoom.playerState.stage, "F_SP108", sizeof(aRoom.playerState.stage) - 1);
    aRoom.playerState.roomNo = 2;
    Check(a->SendGameMessage(MsgType::PlayerState, aRoom), "A announces room 2");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Check(host->roomOwner("F_SP108", 2) == b->selfId(),
        "A arriving second does not take ownership (sticky)");

    // -- 0b) M4.5 review MINOR 1: a cross-stage SceneChange (scene=0 — the
    //    sender's stage changed) must NOT key the room table with the
    //    player's LAST-KNOWN stage + new room (a bogus (F_SP108, 7) owner
    //    entry that would mis-route intents for a frame or two). The crossing
    //    room belongs to the NEW stage; the first new-stage PlayerState
    //    (channel 1, send-window-guaranteed) establishes the entry instead.
    PayloadUnion xStage = {};
    xStage.playerEvent.playerId = a->selfId();
    xStage.playerEvent.eventId = static_cast<u8>(PlayerEventId::SceneChange);
    xStage.playerEvent.data = 7;  // the NEW stage's room
    xStage.playerEvent.scene = 0; // stage changed
    Check(a->SendGameMessage(MsgType::PlayerEvent, xStage),
        "A sends a cross-stage SceneChange (scene=0)");
    // A's aRoom PlayerState is unreliable; pump until the host processed it.
    Check(demo.WaitFor([&] { return host->playerRoom(a->selfId()).room == 2; }, 10000),
        "host processed A's room-2 PlayerState");
    Check(a->SendGameMessage(MsgType::PlayerEvent, xStage),
        "A sends a cross-stage SceneChange (scene=0)");
    // Pump a fixed window so the reliable SceneChange is certainly delivered
    // (its only observable guarantee is that it does NOT move the table).
    const u64 xDeadline = NowMs() + 500;
    while (NowMs() < xDeadline) {
        for (Session* s : demo.live) {
            s->Update();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Check(host->playerRoom(a->selfId()).room == 2,
        "cross-stage SceneChange did not move A in the room table");
    Check(host->roomOwner("F_SP108", 7) == kInvalidPlayerId,
        "no bogus (F_SP108, 7) owner entry from the cross-stage SceneChange");
    PayloadUnion xState = {};
    xState.playerState.playerId = a->selfId();
    std::strncpy(xState.playerState.stage, "F_SP104", sizeof(xState.playerState.stage) - 1);
    xState.playerState.roomNo = 7;
    Check(a->SendGameMessage(MsgType::PlayerState, xState),
        "A's first new-stage PlayerState (F_SP104, 7)");
    Check(demo.WaitFor(
            [&] {
                return host->playerRoom(a->selfId()).room == 7 &&
                       std::strcmp(host->playerRoom(a->selfId()).stage, "F_SP104") == 0;
            },
            10000),
        "new-stage PlayerState establishes (F_SP104, 7)");
    Check(demo.WaitFor([&] { return host->roomOwner("F_SP104", 7) == a->selfId(); }, 10000),
        "A owns the room it first enters on the new stage");
    // A returns to room 2 (same-stage moves mark scene=1).
    Check(a->SendGameMessage(MsgType::PlayerState, aRoom), "A returns to room 2");
    PayloadUnion aSceneBack = {};
    aSceneBack.playerEvent.playerId = a->selfId();
    aSceneBack.playerEvent.eventId = static_cast<u8>(PlayerEventId::SceneChange);
    aSceneBack.playerEvent.data = 2;
    aSceneBack.playerEvent.scene = 1; // same-stage move
    Check(a->SendGameMessage(MsgType::PlayerEvent, aSceneBack),
        "A announces room 2 again (same-stage, scene=1)");
    Check(demo.WaitFor([&] { return host->playerRoom(a->selfId()).room == 2; }, 10000),
        "host sees A back in room 2");

    // -- 1) CombatIntent from A (room 2) routes to the room owner B, NOT to
    //    the host's handler and NOT back to A.
    PayloadUnion intent = {};
    intent.combatIntent.attackerId = a->selfId();
    intent.combatIntent.targetEnemyId = 0x0203;  // stage-placed (room 2)
    intent.combatIntent.atp = 3;
    intent.combatIntent.powerType = 1;
    intent.combatIntent.seq = 1;
    Check(a->SendGameMessage(MsgType::CombatIntent, intent), "A sends CombatIntent");
    Check(demo.WaitFor([&] { return bIntents >= 1; }, 10000),
        "room owner B received A's CombatIntent (routed, not relayed)");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Check(hostIntents == 0, "host did NOT consume the intent it routed to B");
    Check(aIntents == 0, "intent not echoed back to A");

    // -- 2) A moves into the host's room 1; the host-default rule keeps the
    //    host as owner. A's intent in room 1 must reach the host's handler.
    PayloadUnion aRoom1 = {};
    aRoom1.playerState.playerId = a->selfId();
    std::strncpy(aRoom1.playerState.stage, "F_SP108", sizeof(aRoom1.playerState.stage) - 1);
    aRoom1.playerState.roomNo = 1;
    Check(a->SendGameMessage(MsgType::PlayerState, aRoom1), "A moves to room 1");
    // The reliable SceneChange is the room-change authority (the real game
    // sends it before the unreliable states); wait for the HOST's own view of
    // A's room rather than the (already-true) ownership, so the intent below
    // routes on A's NEW room.
    PayloadUnion aScene1 = {};
    aScene1.playerEvent.playerId = a->selfId();
    aScene1.playerEvent.eventId = static_cast<u8>(PlayerEventId::SceneChange);
    aScene1.playerEvent.data = 1;
    aScene1.playerEvent.scene = 1; // same-stage move (M4.5 MINOR 1)
    Check(a->SendGameMessage(MsgType::PlayerEvent, aScene1), "A announces room 1 (reliable)");
    Check(demo.WaitFor([&] { return host->playerRoom(a->selfId()).room == 1; }, 10000),
        "host sees A in room 1");
    Check(host->roomOwner("F_SP108", 1) == 0,
        "host owns room 1 even with a client present (host default)");
    PayloadUnion intent2 = {};
    intent2.combatIntent.attackerId = a->selfId();
    intent2.combatIntent.targetEnemyId = 0x0103;
    intent2.combatIntent.atp = 2;
    intent2.combatIntent.powerType = 1;
    intent2.combatIntent.seq = 2;
    Check(a->SendGameMessage(MsgType::CombatIntent, intent2),
        "A sends CombatIntent in the host's room");
    Check(demo.WaitFor([&] { return hostIntents >= 1; }, 10000),
        "host consumed the intent for its own room");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Check(bIntents == 1, "B (owner of room 2 only) did not receive the room-1 intent");

    // -- 2b) M4.5 MAJOR 2: EnemyEvent is room-scoped like EnemySnapshot. B
    //    (room-2 owner) sends a died event for a room-2 enemy; A is in room 1
    //    now, so it must NOT reach A (and not echo back to B); the host still
    //    consumes it. Before this fix the event was star-relayed and a
    //    cross-stage peer with a coincident (roomNo<<8)|setID mis-killed a
    //    local enemy, spawned the wrong drop and granted the wrong switch.
    PayloadUnion ev2 = {};
    ev2.enemyEvent.enemyId = 0x0203;  // stage-placed room-2 enemy
    ev2.enemyEvent.eventId = static_cast<u8>(EnemyEventId::Died);
    ev2.enemyEvent.data = 0x1E;       // drop table id
    ev2.enemyEvent.flagMask = 0x04;   // save switch that must NOT reach A
    Check(b->SendGameMessage(MsgType::EnemyEvent, ev2), "owner B sends EnemyEvent(died) for room 2");
    Check(demo.WaitFor([&] { return hostEvents >= 1; }, 10000),
        "host consumed B's EnemyEvent");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Check(aEvents == 0, "room-2 died event NOT relayed to the room-1 peer A (room scoping)");
    Check(bEvents == 0, "died event not echoed back to the owner B");

    // -- 2c) M4.6 (capstone MINOR K): the RoomClear receive-gate rows + the
    //    explicit CROSS-STAGE EnemyEvent row. RoomClear carries the BARE room
    //    number; the relay is room-scoped like Died (stage+room), and the
    //    receive side (coop_enemy.cpp) further gates on the local room. These
    //    lock the M4.5 MAJOR-2 fix exactly.
    //    (a) Owner B clears room 2 while A is in room 1: the bit must NOT
    //    reach A (a room-1 peer no-ops).
    PayloadUnion roomClear = {};
    roomClear.enemyEvent.enemyId = 2;  // bare room number
    roomClear.enemyEvent.eventId = static_cast<u8>(EnemyEventId::RoomClear);
    Check(b->SendGameMessage(MsgType::EnemyEvent, roomClear),
        "owner B sends EnemyEvent(RoomClear) for room 2");
    Check(demo.WaitFor([&] { return hostEvents >= 2; }, 10000),
        "host consumed B's RoomClear");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Check(aEvents == 0, "RoomClear room 2 NOT relayed to the room-1 peer A (receive gate no-op)");
    Check(bEvents == 0, "RoomClear not echoed back to the owner B");

    //    (b) CROSS-STAGE row: A moves to (F_SP104, 2) — a stage whose room
    //    number COINCIDES with B's room 2 in F_SP108. B's died event for a
    //    F_SP108 room-2 enemy must NOT reach A: the STAGE half of the
    //    (stage, room) relay key blocks it (the pre-M4.5 star-relay would
    //    have mis-killed A's coincident (2<<8)|setID local enemy, spawned the
    //    wrong drop and granted the wrong switch).
    PayloadUnion xRoom = {};
    xRoom.playerState.playerId = a->selfId();
    std::strncpy(xRoom.playerState.stage, "F_SP104", sizeof(xRoom.playerState.stage) - 1);
    xRoom.playerState.roomNo = 2;
    Check(a->SendGameMessage(MsgType::PlayerState, xRoom), "A moves to (F_SP104, 2)");
    PayloadUnion xScene = {};
    xScene.playerEvent.playerId = a->selfId();
    xScene.playerEvent.eventId = static_cast<u8>(PlayerEventId::SceneChange);
    xScene.playerEvent.data = 2;
    xScene.playerEvent.scene = 0;  // cross-stage move
    Check(a->SendGameMessage(MsgType::PlayerEvent, xScene), "A announces the cross-stage move");
    Check(demo.WaitFor(
            [&] {
                return std::strcmp(host->playerRoom(a->selfId()).stage, "F_SP104") == 0 &&
                       host->playerRoom(a->selfId()).room == 2;
            },
            10000),
        "host sees A in (F_SP104, 2)");
    Check(demo.WaitFor([&] { return host->roomOwner("F_SP104", 2) == a->selfId(); }, 10000),
        "A owns (F_SP104, 2) (first in)");
    PayloadUnion xDied = {};
    xDied.enemyEvent.enemyId = 0x0203;  // stage-placed room-2 enemy in F_SP108
    xDied.enemyEvent.eventId = static_cast<u8>(EnemyEventId::Died);
    xDied.enemyEvent.data = 0x1E;       // drop table id that must NOT spawn on A
    xDied.enemyEvent.flagMask = 0x06;   // save switch that must NOT reach A
    Check(b->SendGameMessage(MsgType::EnemyEvent, xDied),
        "owner B sends died for a F_SP108 room-2 enemy");
    Check(demo.WaitFor([&] { return hostEvents >= 3; }, 10000),
        "host consumed the cross-stage died event");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Check(aEvents == 0,
        "cross-stage died (coincident room 2, different stage) NOT relayed to A");
    Check(bEvents == 0, "cross-stage died not echoed back to the owner B");

    //    (c) A returns to room 1 (F_SP108) so the section-3 snapshot-scoping
    //    checks below still see a room-1 peer.
    PayloadUnion aRoom1b = {};
    aRoom1b.playerState.playerId = a->selfId();
    std::strncpy(aRoom1b.playerState.stage, "F_SP108", sizeof(aRoom1b.playerState.stage) - 1);
    aRoom1b.playerState.roomNo = 1;
    Check(a->SendGameMessage(MsgType::PlayerState, aRoom1b), "A returns to room 1");
    PayloadUnion aScene1b = {};
    aScene1b.playerEvent.playerId = a->selfId();
    aScene1b.playerEvent.eventId = static_cast<u8>(PlayerEventId::SceneChange);
    aScene1b.playerEvent.data = 1;
    aScene1b.playerEvent.scene = 1;  // same-stage move
    Check(a->SendGameMessage(MsgType::PlayerEvent, aScene1b), "A announces room 1 (reliable)");
    Check(demo.WaitFor([&] { return host->playerRoom(a->selfId()).room == 1; }, 10000),
        "host sees A back in room 1");

    // -- 3) EnemySnapshot from owner B (room 2) reaches only room-2 peers.
    //    A is now in room 1, so B's room-2 snapshot must NOT reach A (and
    //    not echo back to B); the host still consumes it.
    PayloadUnion snap = {};
    snap.enemySnapshot.enemyId = 0x0205;
    snap.enemySnapshot.type = 0x01AF;
    snap.enemySnapshot.hp = 42;
    snap.enemySnapshot.maxHp = 100;
    Check(b->SendGameMessage(MsgType::EnemySnapshot, snap), "owner B sends EnemySnapshot");
    Check(demo.WaitFor([&] { return hostSnapshots >= 1; }, 10000),
        "host consumed B's EnemySnapshot");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Check(aSnapshots == 0,
        "room-2 snapshot NOT relayed to a room-1 peer (same-room scoping)");
    Check(bSnapshots == 0, "snapshot not echoed back to the owner");

    // -- 4) A moves back to room 2: now B's snapshot reaches A.
    Check(a->SendGameMessage(MsgType::PlayerState, aRoom), "A returns to room 2");
    PayloadUnion aScene2 = {};
    aScene2.playerEvent.playerId = a->selfId();
    aScene2.playerEvent.eventId = static_cast<u8>(PlayerEventId::SceneChange);
    aScene2.playerEvent.data = 2;
    aScene2.playerEvent.scene = 1; // same-stage move (M4.5 MINOR 1)
    Check(a->SendGameMessage(MsgType::PlayerEvent, aScene2), "A announces room 2 (reliable)");
    Check(demo.WaitFor([&] { return host->playerRoom(a->selfId()).room == 2; }, 10000),
        "host sees A back in room 2");
    Check(demo.WaitFor([&] { return host->roomOwner("F_SP108", 2) == b->selfId(); }, 10000),
        "B still owns room 2 (sticky across A's moves)");
    PayloadUnion snap2 = {};
    snap2.enemySnapshot.enemyId = 0x0207;
    snap2.enemySnapshot.type = 0x01AF;
    Check(b->SendGameMessage(MsgType::EnemySnapshot, snap2), "owner B sends another EnemySnapshot");
    Check(demo.WaitFor([&] { return aSnapshots >= 1; }, 10000),
        "room-2 snapshot relayed to the room-2 peer A");
    // ... and now B's room-2 died event DOES reach A (both in room 2).
    PayloadUnion ev3 = {};
    ev3.enemyEvent.enemyId = 0x0204;
    ev3.enemyEvent.eventId = static_cast<u8>(EnemyEventId::Died);
    ev3.enemyEvent.data = 0x1F;
    ev3.enemyEvent.flagMask = 0x05;
    Check(b->SendGameMessage(MsgType::EnemyEvent, ev3), "owner B sends another EnemyEvent(died)");
    Check(demo.WaitFor([&] { return aEvents >= 1; }, 10000),
        "room-2 died event relayed to the room-2 peer A");

    // Capstone MINOR K (M4.6): the RoomClear receive gate LANDS once A is a
    // room-2 peer — the same bit that no-oped from room 1 in 2c(a).
    Check(b->SendGameMessage(MsgType::EnemyEvent, roomClear),
        "owner B sends RoomClear for room 2 again");
    Check(demo.WaitFor([&] { return aEvents >= 2; }, 10000),
        "room-2 RoomClear relayed to the room-2 peer A (receive gate lands)");

    // -- 5) CombatResult from owner B reaches everyone except the origin
    //    (star relay; the host relays to the other joined peer A).
    PayloadUnion result = {};
    result.combatResult.targetEnemyId = 0x0205;
    result.combatResult.damage = 4;
    result.combatResult.newHp = 38;
    result.combatResult.outcome = static_cast<u8>(CombatOutcome::Hit);
    result.combatResult.attackerId = a->selfId();
    result.combatResult.seq = 1;
    Check(b->SendGameMessage(MsgType::CombatResult, result), "owner B sends CombatResult");
    Check(demo.WaitFor([&] { return aResults >= 1 && cResults >= 1; }, 10000),
        "CombatResult from a client owner reaches the other peer + host (star relay)");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Check(bResults == 0, "origin B does not receive its own CombatResult back");

    // -- 6) Ownership transfer on leave: B (owner of room 2) leaves; A is
    //    still in room 2 and must take over.
    PayloadUnion leave = {};
    leave.playerLeave.playerId = b->selfId();
    Check(b->SendGameMessage(MsgType::PlayerLeave, leave), "B sends PlayerLeave");
    Check(demo.WaitFor([&] { return host->roomOwner("F_SP108", 2) == a->selfId(); }, 10000),
        "room 2 ownership transferred to A after the owner left");

    for (Session* s : demo.live) {
        s->Stop();
    }
    demo.live.clear();
    host->Stop();
}

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

/// Entity-id stability across ownership transfer: stage-placed keys are a
/// pure function of stage data (identical for any owner), dynamic ids are
/// owner-major so two owners' spaces never collide after a takeover.
void RunM4EntityStabilityCheck() {
    std::printf("m4: entity-id stability across ownership transfer\n");
    using namespace dusk::coop::enemy;

    // Stage-placed keys: identical on every machine, independent of who owns
    // the room (the transfer changes nothing — a map transfer, not renumber).
    Check(StageEntityId(2, 3) == ((2 << 8) | 3), "stage key packs (room, setID)");
    Check(StageEntityId(2, 3) == StageEntityId(2, 3),
        "stage key is a pure function of stage data (stable across owners)");
    Check(StageEntityId(2, 3) != StageEntityId(3, 3), "different rooms key differently");
    Check(StageEntityId(4, 0xFFFF) == kInvalidEnemyId, "dynamic spawns are not stage-keyed");
    Check(StageEntityId(-1, 3) == kInvalidEnemyId, "no room -> no key");

    // Dynamic ids: owner-major — two owners can never collide, and ids stay
    // clear of the stage-key space.
    const u16 dynA = DynamicEntityId(3, 1);
    const u16 dynB = DynamicEntityId(5, 1);
    Check((dynA & kDynamicIdBase) != 0, "dynamic ids live above the stage-key space");
    Check(dynA != dynB, "different owners allocate disjoint dynamic id spaces");
    Check(dynA != StageEntityId(2, 3) && dynB != StageEntityId(2, 3),
        "dynamic ids never collide with stage keys");
    const u16 dynA2 = DynamicEntityId(3, 2);
    Check(dynA != dynA2, "the same owner's counter advances");
    Check(DynamicEntityId(3, 0x1234) == DynamicEntityId(3, 0x1234),
        "same owner + same counter -> same id (deterministic)");
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

// ---------------------------------------------------------------------------
// M4 — LAN discovery (HostAnnounce)
// ---------------------------------------------------------------------------

void RunM4DiscoveryCheck() {
    std::printf("m4: LAN discovery HostAnnounce (loopback)\n");
    using namespace dusk::net::discovery;

    Listener listener;
    listener.Start();
    const u64 before = AnnouncesReceived();
    Announcer announcer;
    // M4.5 (review MINOR 6): seed the player count before Start so the very
    // first datagram never advertises 0 (ThreadMain's first send already
    // reads the atomic). This also makes the players==2 check below free of
    // the old SetPlayers-after-Start race.
    announcer.SetPlayers(2);
    announcer.Start("Dusklight Test Session", 44770, 4);

    // The announcer broadcasts every 2 s (LAN + loopback targets); the
    // loopback send is deterministic on one machine, so a few seconds of
    // polling is ample.
    std::vector<DiscoveredSession> found;
    const u64 deadline = NowMs() + 8000;
    while (NowMs() < deadline) {
        found = listener.Sessions();
        if (!found.empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    Check(!found.empty(), "listener discovered the announcing host on loopback");
    if (!found.empty()) {
        const auto& ds = found.front();
        Check(ds.port == 44770, "announce carries the session port");
        Check(ds.players == 2 && ds.maxPlayers == 4, "announce carries players/max");
        Check(std::strcmp(ds.name, "Dusklight Test Session") == 0,
            "announce carries the session name");
    }
    Check(AnnouncesReceived() >= before + 1, "accepted datagrams counted");

    announcer.Stop();
    listener.Stop();
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
    RunM2RelayPolicyCheck();
    RunM3TimeWeatherCheck();
    RunM35TimeWeatherFixCheck();
    RunM4OwnershipTableCheck();
    RunM4RoomRoutingCheck();
    RunM46SessionRestartCheck();
    RunM4WorldStageCheck();
    RunM4EntityStabilityCheck();
    RunM4DiscoveryCheck();

    dusk::net::shutdown();

    if (g_failures == 0) {
        std::printf("PASS: all checks succeeded\n");
        return 0;
    }
    std::printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
