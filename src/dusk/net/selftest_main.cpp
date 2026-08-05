/**
 * \file selftest_main.cpp
 * M0 LAN handshake demo + protocol/cadence self-test.
 *
 * Run (build dir):  ./dusk_net_selftest
 * Exits 0 on success, 1 on any failure.
 *
 * Covers, in one process over loopback (127.0.0.1):
 *   - wire round-trip (serialize -> deserialize -> re-serialize) for all 15
 *     message types in 00-network.md §5, plus malformed-input rejection;
 *   - NetClock 60 Hz cadence (exact interval spacing, catch-up);
 *   - host/client session lifecycle: JoinRequest -> JoinAccept + WorldInit,
 *     JoinReject (version mismatch, session full), PlayerLeave relay, and
 *     host SessionEnd;
 *   - clean shutdown: every transport thread joined, every ENet host
 *     destroyed (leak-free exit).
 *
 * The net module logs through aurora::Module; this standalone binary provides
 * the aurora logging globals (mirroring extern/aurora/lib/logging.cpp) so it
 * links without pulling the whole aurora runtime.
 */

#include "dusk/net/clock.h"
#include "dusk/net/protocol.h"
#include "dusk/net/session.h"

#include <aurora/aurora.h>
#include <aurora/lib/logging.hpp>

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
// Protocol round-trip
// ---------------------------------------------------------------------------

const char* StageName() {
    return "F_SP108";
}

Message MakeMessage(MsgType type) {
    Message m;
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
        p.time.phase = 1234567;
        p.time.elapsedMs = 4321;
        p.weather.id = static_cast<u8>(WeatherId::Rain);
        p.weather.intensity = 42;
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
        p.playerStateCount = 2;
        p.enemyStateCount = 4;
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
        p.scene = 9;
        p.form = 1;
        p.movementFlags = 0x55;
        p.jointCount = 40;
        p.cosmetics[0] = 1;
        p.cosmetics[1] = 2;
        p.cosmetics[2] = 3;
        p.itemAction = 4;
        p.invincibility = 17;
        p.stateFlags = 0xDEADBEEF;
        p.pos = Vec3f{1.5f, -2.25f, 300.125f};
        p.rot = Vec3s16{100, 200, 300};
        p.upperLimbRot = Vec3s16{-1, -2, -3};
        for (u8 i = 0; i < kMaxJoints; ++i) {
            p.joints[i] = Vec3s16{static_cast<s16>(i), static_cast<s16>(i * 2), static_cast<s16>(-i)};
        }
        break;
    }
    case MsgType::PlayerEvent: {
        auto& p = m.payload.playerEvent;
        p.playerId = 1;
        p.eventId = static_cast<u8>(PlayerEventId::FormChange);
        p.scene = 4;
        p.data = 0xF0F0F0F0;
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
        break;
    }
    case MsgType::EnemyEvent: {
        auto& p = m.payload.enemyEvent;
        p.enemyId = 88;
        p.data = 99;
        p.eventId = static_cast<u8>(EnemyEventId::Died);
        p.flags = 0x0F;
        break;
    }
    case MsgType::CombatIntent: {
        auto& p = m.payload.combatIntent;
        p.attackerId = 1;
        p.attackKind = 5;
        p.targetPlayerId = kInvalidPlayerId;
        p.targetEnemyId = 77;
        p.damage = 4;
        p.seq = 12345;
        p.position = Vec3f{0.5f, 0.5f, 0.5f};
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
        p.phase = 999999;
        p.elapsedMs = 16;
        break;
    }
    case MsgType::TimeEvent: {
        auto& p = m.payload.timeEvent;
        p.eventId = static_cast<u8>(TimeEventId::Dawn);
        p.timePhase = 43210;
        break;
    }
    case MsgType::WeatherChange: {
        auto& p = m.payload.weatherChange;
        p.weatherId = static_cast<u8>(WeatherId::Storm);
        p.intensity = 100;
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
    std::printf("protocol: round-trip all 15 message types\n");
    for (u16 t = static_cast<u16>(MsgType::JoinRequest); t <= static_cast<u16>(MsgType::WeatherChange);
         ++t)
    {
        const auto type = static_cast<MsgType>(t);
        Check(RoundTrip(type), WireSize(type) > 0 ? "round-trip ok" : "round-trip ok (size 0)");
    }
    Check(WireSize(MsgType::JoinAccept) == 1 + 3 + 20 + 8 + 4 + kMaxLocalPlayers * 36,
        "JoinAccept wire size");
    Check(WireSize(MsgType::PlayerState) == 11 + 4 + 12 + 6 + 6 + kMaxJoints * 6,
        "PlayerState wire size");
    Check(ChannelFor(MsgType::PlayerState) == kChannelUnreliable &&
              ChannelFor(MsgType::JoinRequest) == kChannelReliable,
        "channel mapping (snapshots unreliable, control reliable)");

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
    Session host;
    SessionConfig hostCfg;
    hostCfg.port = 0;  // ephemeral; the demo reads boundPort()
    hostCfg.name = "Test Host";
    hostCfg.maxPlayers = 2;
    std::strncpy(hostCfg.stage.stage, "F_SP108", sizeof(hostCfg.stage.stage) - 1);
    hostCfg.stage.room = 2;
    hostCfg.stage.layer = 1;
    hostCfg.stage.point = 7;
    Check(host.StartHost(hostCfg), "host session starts (Listening)");
    demo.live.push_back(&host);
    const u16 port = host.boundPort();
    Check(port != 0, "host bound an ephemeral port");
    std::printf("    host listening on 127.0.0.1:%u\n", port);

    // -- client A joins --
    {
        Session a;
        SessionConfig cfg;
        cfg.joinHost = "127.0.0.1";
        cfg.port = port;
        cfg.name = "Player A";
        cfg.version = kProtocolVersion;
        Check(a.StartClient(cfg), "client A starts (Connecting)");
        demo.live.push_back(&a);

        Check(demo.WaitFor([&] { return a.state() == SessionState::Joined; }, 10000),
            "A reaches Joined (JoinRequest -> JoinAccept)");
        Check(a.selfId() == 1, "A assigned PlayerId 1");
        Check(demo.WaitFor([&] { return PresentCountOf(host) == 2; }, 10000),
            "host roster has 2 players (host + A)");
        Check(std::strcmp(a.worldStage().stage, "F_SP108") == 0 && a.worldStage().room == 2 &&
                  a.worldStage().point == 7,
            "A received WorldInit stage/room/point from the host");
        Check(a.roster()[0].present && std::strcmp(a.roster()[0].name, "Test Host") == 0,
            "A sees the host in the roster");

        // -- client B: wrong protocol version -> reject --
        Session b;
        SessionConfig bCfg;
        bCfg.joinHost = "127.0.0.1";
        bCfg.port = port;
        bCfg.name = "Player B (old)";
        bCfg.version = kProtocolVersion + 100;
        Check(b.StartClient(bCfg), "client B starts");
        demo.live.push_back(&b);
        Check(demo.WaitFor([&] { return b.state() == SessionState::Rejected; }, 10000),
            "B rejected (version mismatch)");
        Check(std::strstr(b.rejectReasonName(), "version") != nullptr,
            "B rejection reason is version mismatch");
        b.Stop();
        demo.live.erase(std::remove(demo.live.begin(), demo.live.end(), &b), demo.live.end());

        // -- client C: roster full (maxPlayers=2) -> reject --
        Session c;
        SessionConfig cCfg;
        cCfg.joinHost = "127.0.0.1";
        cCfg.port = port;
        cCfg.name = "Player C";
        cCfg.version = kProtocolVersion;
        Check(c.StartClient(cCfg), "client C starts");
        demo.live.push_back(&c);
        Check(demo.WaitFor([&] { return c.state() == SessionState::Rejected; }, 10000),
            "C rejected (session full)");
        Check(std::strstr(c.rejectReasonName(), "full") != nullptr,
            "C rejection reason is session full");
        c.Stop();
        demo.live.erase(std::remove(demo.live.begin(), demo.live.end(), &c), demo.live.end());

        // -- A leaves: PlayerLeave relayed, host roster shrinks --
        Check(a.state() == SessionState::Joined, "A still joined before leaving");
        a.Stop();
        Check(a.state() == SessionState::Ended, "A session Ended after Stop");
        Check(demo.WaitFor([&] { return PresentCountOf(host) == 1; }, 10000),
            "host roster back to 1 (host) after A's PlayerLeave");
        demo.live.erase(std::remove(demo.live.begin(), demo.live.end(), &a), demo.live.end());
    }

    // -- client D joins, then the host ends the session --
    {
        Session d;
        SessionConfig cfg;
        cfg.joinHost = "127.0.0.1";
        cfg.port = port;
        cfg.name = "Player D";
        cfg.version = kProtocolVersion;
        Check(d.StartClient(cfg), "client D starts");
        demo.live.push_back(&d);
        Check(demo.WaitFor([&] { return d.state() == SessionState::Joined; }, 10000),
            "D reaches Joined (PlayerId reuses slot 1)");
        Check(d.selfId() == 1, "D assigned PlayerId 1");

        host.Stop();
        Check(host.state() == SessionState::Ended, "host session Ended after Stop");
        Check(demo.WaitFor([&] { return d.state() == SessionState::Ended; }, 10000),
            "D observes SessionEnd(HostLeft) and goes Ended");
        d.Stop();
        demo.live.erase(std::remove(demo.live.begin(), demo.live.end(), &d), demo.live.end());
    }

    demo.live.erase(std::remove(demo.live.begin(), demo.live.end(), &host), demo.live.end());
    Check(host.sessionFrames() > 0, "host session pumped frames");
}

}  // namespace

int main() {
    std::printf("dusk_net_selftest: M0 network layer (ENet %d.%d.%d)\n", ENET_VERSION_MAJOR,
        ENET_VERSION_MINOR, ENET_VERSION_PATCH);

    if (enet_initialize() != 0) {
        std::fprintf(stderr, "enet_initialize failed\n");
        return 1;
    }

    RunProtocolChecks();
    RunClockChecks();
    RunHandshakeDemo();

    enet_deinitialize();

    if (g_failures == 0) {
        std::printf("PASS: all checks succeeded\n");
        return 0;
    }
    std::printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
