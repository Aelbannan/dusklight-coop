#pragma once

/**
 * \file protocol.h
 * Network co-op wire protocol (docs/design/mod-coop/00-network.md §5).
 *
 * Wire format for every message:
 *   u16 type   — MsgType, little-endian
 *   u16 size   — payload size in bytes, little-endian
 *   payload    — fixed-size little-endian struct, hand-written serializers
 *
 * All payloads are fixed-size (roster is a fixed 8-entry array, strings are
 * fixed-capacity NUL-padded arrays, WorldInit is stage+roster only — no
 * full-state sections), so the hot path never allocates and a received
 * message can be validated against its expected size in one check.
 * DeserializeMessage stays exact-size by design: variable-length messages
 * are rejected. Channel 0 is reliable (control/events/combat), channel 1 is
 * unreliable sequenced (snapshots), per 00-network.md §1.
 *
 * The pose fields in PlayerState follow 00-network.md §5's joint-list format.
 * Rev 3 D4 (raw matrix pose) refines the same message in M1; only the
 * serializer body and this struct change, never the envelope.
 */

#include <dolphin/mtx.h>
#include <dolphin/types.h>

#include <array>
#include <cstring>

namespace dusk::net {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Bump when the byte layout of any message changes; JoinRequest carries it
/// and mismatches are rejected with JoinReject(VersionMismatch).
///
/// v2 (M0.5): WorldInit dropped its (always-zero) count-prefixed state
/// sections — it is now fixed stage+roster only. The zero-init and semantic
/// validation changes do not alter the wire layout and did not bump.
///
/// v3 (M1): PlayerState moved from the provisional joint-list layout to Rev 3
/// D4's raw-matrix pose (per-joint Mtx table + scale flags + baseTR + face +
/// roomNo + stateFlags byte, ~2 KB with 3x4 Mtx); PlayerEvent gained an
/// extended data2 field for item-joint payloads.
///
/// v4 (M2): EnemySnapshot gained per-enemy `speed` (velocity, for knockback /
/// anim hints) and a per-type `semantics` tag (hp | hitCount); CombatIntent
/// was rebuilt to carry the RAW attack fields the sim owner needs to reproduce
/// damage deterministically (atp, powerType, hitType, AtType bits) plus the
/// contract hitPos/attackerPos — see m2-design-notes.md §1.
///
/// v5 (M3): TimeSync/TimeEvent/WeatherChange moved from the M0 placeholder
/// layouts (phase f32/u32 opaque) to the absolute-phase contract of
/// 04-time-weather.md §4 (time f32 0..360 + day + rate + flags; event +
/// time + day; mode + thunder + intensity + colpat). JoinAccept/WorldInit
/// carry the same TimeStateInfo/WeatherStateInfo so a mid-game joiner starts
/// with the host's sky.
///
/// v6 (M4): added RoomOwnershipMsg (reliable, host->all) carrying one room
/// owner assignment (stage + room + owner PlayerId) — the distributed
/// authority map of docs/design/network.md §6. The host owns the table
/// (sticky first-in-room ownership, host-defaults-own-its-room, transfer on
/// leave/disconnect), broadcasts it on every change and to each joiner, and
/// uses it to route CombatIntent to the room's owner and scope EnemySnapshot
/// fan-out to the sender's room. Message destinations, not a separate routing
/// layer (00-network.md §2).
///
/// M4.5 (capstone MINOR 4, review-full-glm-5.2.md): also redefined the
/// EXISTING PlayerEventMsg.scene byte as the same-stage flag (1 = same-stage
/// move, 0 = cross-stage / not a SceneChange — the host's room-table sniff
/// keys (last-known stage, new room) on it). Semantic change to an existing
/// field, NO layout change — hence no bump here; M5 bumps to v7 (it adds wire
/// fields: spawn params, horse channel).
constexpr u16 kProtocolVersion = 6;

/// Session-wide player id space (0..kMaxLocalPlayers-1), per
/// docs/design/network.md §3.
constexpr u8 kMaxLocalPlayers = 8;

/// ENet channels (00-network.md §1): reliable control vs unreliable snapshots.
constexpr u8 kChannelReliable = 0;
constexpr u8 kChannelUnreliable = 1;

/// Fixed-capacity string limits (include the NUL terminator on the wire).
constexpr u8 kMaxNameLength = 32;
constexpr u8 kMaxStageNameLength = 16;

/// TP Link joint count for pose sync; plan risk R11: human 40 / wolf 37+.
/// jointCount in PlayerState marks how many of the fixed kMaxJoints entries
/// are meaningful (the rest are written zeroed so the wire size stays fixed).
constexpr u8 kMaxJoints = 40;

/// Largest message the transport ring slots carry; fits the raw-matrix pose
/// (~2.0 KB — 2017 B payload + 4 B envelope, capstone MINOR I) plus envelope
/// with room to spare.
constexpr u16 kMaxMessageSize = 4096;

constexpr u8 kInvalidPlayerId = 0xFF;
constexpr u8 kAnySlot = 0xFF;

/// Session-wide player id (0..kMaxLocalPlayers-1), assigned by the host.
using PlayerId = u8;

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

enum class MsgType : u16 {
    JoinRequest = 1,
    JoinAccept = 2,
    JoinReject = 3,
    PlayerLeave = 4,
    SessionEnd = 5,
    WorldInit = 6,
    PlayerState = 7,
    PlayerEvent = 8,
    EnemySnapshot = 9,
    EnemyEvent = 10,
    CombatIntent = 11,
    CombatResult = 12,
    TimeSync = 13,
    TimeEvent = 14,
    WeatherChange = 15,
    RoomOwnership = 16,
};

enum class JoinRejectReason : u8 {
    SessionFull = 0,
    VersionMismatch = 1,
    InvalidSlot = 2,
    /// Client-local only (join deadline expired); never sent on the wire.
    Timeout = 3,
};

enum class SessionEndReason : u8 {
    HostLeft = 0,
    Kicked = 1,
    Shutdown = 2,
    /// Client-local only: the ENet connection dropped without a SessionEnd
    /// (host crashed / network loss); never sent on the wire.
    ConnectionLost = 3,
};

enum class PlayerStateId : u8 {
    Connected = 0,
    Playing = 1,
};

enum class PlayerEventId : u8 {
    FormChange = 0,      // data: form (0 human / 1 wolf)
    Mount = 1,           // (reserved; horse entity channel is M5)
    Dismount = 2,        // (reserved)
    Respawn = 3,         // (reserved)
    SceneChange = 4,     // data: roomNo
    Equip = 5,           // data: equipItem u16 | selectItemId u8 | clothes u8;
                         //       data2: leftItemJnt u16 | rightItemJnt u16
    AttentionChange = 6, // data: session entity id of the lock target
                         //       (kInvalidPlayerId/0xFFFF = none or local-only)
};

enum class EnemyEventId : u8 {
    Spawned = 0,
    Died = 1,
    RoomClear = 2,
    BossPhase = 3,
};

enum class CombatOutcome : u8 {
    Hit = 0,
    Miss = 1,
    Blocked = 2,
    Died = 3,
    /// Sim owner refused the intent (out of range, invalid target, friendly
    /// fire off — network.md §7).
    Rejected = 4,
};

/// Day-clock boundary events (04-time-weather.md §4.2): NEW_DAY fires on the
/// 360 wrap (host mDate++ + dKankyo_DayProc), DAWN/DUSK on the upward
/// crossing of phase 90 / 285 (dKy_daynight_check edges).
enum class TimeEventId : u8 {
    NewDay = 0,
    Dawn = 1,
    Dusk = 2,
};

/// Semantic weather mode (04-time-weather.md §4.3). The host derives it from
/// the live sky state (dice machine / kytag06 / snow); clients use its
/// canonical per-mode raincnt target for the local ramp.
enum class WeatherMode : u8 {
    Clear = 0,
    Cloudy = 1,
    RainLight = 2,   // raincnt ~40, colpat 1
    RainHeavy = 3,   // raincnt ~250, colpat 2
    ThunderLight = 4, // thunder on + colpat 1
    ThunderHeavy = 5, // thunder on + colpat 2
    Snow = 6,         // mSnowCount 0..500 (Snowpeak stages)
};

// ---------------------------------------------------------------------------
// Time/weather constants (04-time-weather.md §1.2/§4.1)
// ---------------------------------------------------------------------------

/// TimeSync rate bucket: the client replicates the absolute phase by
/// `ratePerTick * simTicks` between absolute syncs.
constexpr u8 kTimeRateFrozen = 0;  // no advance (event/message/tag/room-gate)
constexpr u8 kTimeRateNormal = 1;  // 0.012 / sim tick
constexpr u8 kTimeRateFast = 2;    // 1.0 / tick (wolf-howl fast-forward)
constexpr u8 kTimeRatePond2x = 3;  // Fishing Pond / Hena's Hut double-advance

/// TimeSync flags bit 0: the host's twilight (darkworld) clock is active —
/// its daytime is pinned to 0 and the synced value is fixed twilight lighting
/// (04-time-weather.md §5.5).
constexpr u8 kTimeFlagDarkworld = 1 << 0;

// ---------------------------------------------------------------------------
// Primitives (fixed layout; mirror cXyz / Vec3s but self-contained)
// ---------------------------------------------------------------------------

struct Vec3f {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
};

struct Vec3s16 {
    s16 x = 0;
    s16 y = 0;
    s16 z = 0;
};

// ---------------------------------------------------------------------------
// Payload structs (one per MsgType; all fixed-size)
// ---------------------------------------------------------------------------

/// One roster entry. present=0 marks an empty slot; name is NUL-padded.
struct PlayerInfo {
    u8 playerId = kInvalidPlayerId;
    u8 present = 0;
    u8 state = 0;  // PlayerStateId
    u8 reserved = 0;
    char name[kMaxNameLength] = {};
};

struct StageInfo {
    char stage[kMaxStageNameLength] = {};  // e.g. "F_SP108", NUL-padded
    s8 room = 0;
    s8 layer = -1;
    s16 point = 0;
};

/// Absolute day-clocked state (04-time-weather.md §4.1): f32 phase 0..360
/// (15 units = 1 hour), save day counter, advance-rate bucket, flags.
struct TimeStateInfo {
    f32 time = 0.0f;  // absolute phase 0..360
    u16 day = 0;      // dComIfGs_getDate()
    u8 rate = 0;      // kTimeRate*
    u8 flags = 0;     // kTimeFlag* bits
};

/// Sky state (04-time-weather.md §4.3): semantic mode + mThunderEff.mMode +
/// intensity (raincnt 0..250 or mSnowCount 0..500) + mColpatWeather.
struct WeatherStateInfo {
    u8 mode = 0;       // WeatherMode
    u8 thunder = 0;    // mThunderEff.mMode (0/1)
    u16 intensity = 0; // raincnt (rain modes) or mSnowCount (Snow)
    u8 colpat = 0;     // mColpatWeather: 0 clear | 1 cloudy/light | 2 heavy/storm
    u8 pad = 0;
};

struct JoinRequestMsg {
    u32 version = 0;   // must equal kProtocolVersion
    u8 requestedSlot = kAnySlot;
    u8 reserved[3] = {};
    char name[kMaxNameLength] = {};
};

struct JoinAcceptMsg {
    u8 assignedPlayerId = kInvalidPlayerId;
    u8 reserved[3] = {};
    StageInfo stage;
    TimeStateInfo time;
    WeatherStateInfo weather;
    std::array<PlayerInfo, kMaxLocalPlayers> roster;
};

struct JoinRejectMsg {
    u8 reason = 0;  // JoinRejectReason
    u8 reserved[3] = {};
};

struct PlayerLeaveMsg {
    u8 playerId = kInvalidPlayerId;
    u8 reserved[3] = {};
};

struct SessionEndMsg {
    u8 reason = 0;  // SessionEndReason
    u8 reserved[3] = {};
};

/// Sent to a joining client right after JoinAccept (00-network.md §4). Fixed
/// size by contract: stage + time + weather + roster. M1's snapshot-on-join is
/// a burst of ordinary per-frame PlayerState / EnemySnapshot messages sent
/// right after WorldInit — no count-prefixed full-state sections live here.
/// M3 (v5): carries the host's current time/weather so a mid-game joiner
/// starts with the host's sky; the roster-refresh broadcast on later joins
/// keeps already-joined peers' sky targets current at the same time.
struct WorldInitMsg {
    StageInfo stage;
    TimeStateInfo time;
    WeatherStateInfo weather;
    std::array<PlayerInfo, kMaxLocalPlayers> roster;
};

/// PlayerState semantic bits (02-player-state.md §2.1). Bit 5 (player-no-draw)
/// is never transmitted — a hidden Link keeps sending its pose.
constexpr u8 kPlayerStateFlagRiding = 1 << 0;        // mRideStatus != 0
constexpr u8 kPlayerStateFlagInvuln = 1 << 1;        // mDamageTimer > 0
constexpr u8 kPlayerStateFlagSubjectivity = 1 << 2;  // mProcID == PROC_SUBJECTIVITY
constexpr u8 kPlayerStateFlagDowned = 1 << 3;        // downed/dead local life state
constexpr u8 kPlayerStateFlagDemo = 1 << 4;          // mDemo.getDemoType() != 0

/// Per-frame pose sync (00-network.md §5 PlayerState; Rev 3 D4 raw-matrix
/// pose, M1). The sender's J3DMtxBuffer per-joint anmMtx table is copied
/// verbatim so a puppet renders the exact blended/callback-baked pose with
/// zero animation-state coupling (02-player-state.md §1.3). Only the first
/// jointCount joints are meaningful; the rest are wire-zeroed so the wire
/// size stays fixed.
struct PlayerStateMsg {
    u8 playerId = kInvalidPlayerId;
    s8 roomNo = 0;             // current.roomNo — same-scene/room scoping
    char stage[kMaxStageNameLength] = {};  // current stage (e.g. "F_SP103") — the
                               // puppet's hidden gate needs stage+room: room
                               // numbers are not unique across stages (spring /
                               // house interiors), so room-only scoping leaks a
                               // remote who left the stage into our view
    u8 form = 0;               // 0 human / 1 wolf (checkWolf())
    u8 stateFlags = 0;         // kPlayerStateFlag_* bits
    u8 jointCount = 0;         // 0..kMaxJoints (semantic-validated at parse)
    u8 scaleFlags[(kMaxJoints + 7) / 8] = {};  // one bit per joint (setScaleFlag)
    s16 yaw = 0;               // shape_angle.y
    s16 pitch = 0;             // mBodyAngle.x
    u16 faceBckIdx = 0;        // mFaceBckHeap.getIdx()
    u16 faceBtpIdx = 0;        // mFaceBtpHeap.getIdx()
    s16 faceFrame = 0;         // face frame ctrl frame
    u8 reserved = 0;
    Vec3f pos;                 // current.pos
    Mtx baseTR;                // mpLinkModel->getBaseTRMtx() — exact world placement
    Mtx joints[kMaxJoints];    // per-joint getAnmMtx(j), root-relative
};

struct PlayerEventMsg {
    u8 playerId = kInvalidPlayerId;
    u8 eventId = 0;  // PlayerEventId
    /// PlayerEventId::SceneChange only (M4.5 review MINOR 1): 1 = the move is
    /// WITHIN the current stage (same stage, new room), 0 = the stage itself
    /// changed (cross-stage) or not a SceneChange. The host's room-ownership
    /// sniff keys (last-known stage, new room), which is valid for same-stage
    /// moves only — a cross-stage SceneChange must not create a bogus
    /// (oldStage, newRoom) entry.
    u8 scene = 0;
    u8 reserved = 0;
    u32 data = 0;   // event-specific payload (see PlayerEventId)
    u32 data2 = 0;  // extended event payload (M1: item joints)
};

struct EnemySnapshotMsg {
    u16 enemyId = 0xFFFF;  // session-unique per room instance
    // Capstone MINOR 2 (review-full-glm-5.2.md MINOR 2): `type` (procName) is
    // populated on the host and serialized but INTENTIONALLY never read by
    // the client apply path (the client already has the local actor with its
    // own type). M5 decides wire-vs-drop; do NOT change behavior.
    u16 type = 0;          // procName / profile id
    u16 hp = 0;
    u16 maxHp = 0;
    // Capstone MINOR 2: `aggro` (the nearest-player hint) is populated on the
    // host (resolveNearestPlayer) and serialized but has NO consumer on the
    // receive side in v1 (a frozen puppet doesn't aggro). M5 decides
    // wire-vs-drop (a consumer would be enemy-attack targeting display).
    u8 aggro = kInvalidPlayerId;  // target player id; kInvalidPlayerId = none
    u8 flags = 0;                 // dead / downed / wolf-bitten + boss-phase
    s16 angle = 0;                // shape_angle.y
    u32 anim = 0;  // per-type packed: action id (u16) + anim/model frame (u16)
    Vec3f pos;     // current.pos
    Vec3f speed;   // current velocty (knockback / anim hints)
    u8 semantics = 0;          // DamageSemantics from the whitelist adapter
    u8 reserved[3] = {};
};

struct EnemyEventMsg {
    u16 enemyId = 0xFFFF;
    u16 data = 0;    // event-specific: drop table id (Died)
    u8 eventId = 0;  // EnemyEventId
    u8 flags = 0;    // kEnemyEventFlag_* bits
    /// Per-player save switch to grant on death (dComIfGs_onSwitch), 0xFF =
    /// none. Carried for every died event: bosses grant their story switch
    /// (D9), regular enemies grant their room switch (enemy-caused world
    /// changes ride the enemy channel — network.md §5).
    u8 flagMask = 0xFF;
    u8 reserved[3] = {};
};

struct CombatIntentMsg {
    u8 attackerId = kInvalidPlayerId;
    u8 powerType = 0;  // the target enemy's mPowerType (validation vs owner)
    u8 hitType = 0;    // HIT_TYPE_* (at_power_check output on the client)
    u8 targetPlayerId = kInvalidPlayerId;  // friendly-fire target (v1: rejected)
    u16 targetEnemyId = 0xFFFF;
    u8 atp = 0;             // raw At collider atp (the enemy's own handler scales it)
    u8 reserved = 0;
    u16 computedPower = 0;  // client's locally-computed damage (informational;
                            // owner reproduces deterministically — m2-design-notes §1)
    u32 seq = 0;            // attacker request counter, echoed by CombatResult
    u32 atType = 0;         // At collider mType bits (cCcD_ObjAtType)
    Vec3f hitPos;           // SetAtTgGObjInf contact point
    Vec3f attackerPos;      // attacker current.pos (range validation)
};

struct CombatResultMsg {
    u16 targetEnemyId = 0xFFFF;
    u16 damage = 0;
    u16 newHp = 0;
    u8 outcome = 0;   // CombatOutcome
    u8 attackerId = kInvalidPlayerId;
    u32 seq = 0;
};

// ---------------------------------------------------------------------------
// Time & weather messages (04-time-weather.md §4; M3 absolute-phase contract)
// ---------------------------------------------------------------------------

/// Unreliable-sequenced, 1 Hz. Absolute phase self-corrects; the rate lets
/// clients advance by `ratePermTick * simTicks` between syncs (frozen during
/// events/messages/time-control tags — rate=0).
struct TimeSyncMsg {
    f32 time = 0.0f;  // absolute phase 0..360
    u16 day = 0;      // dComIfGs_getDate()
    u8 rate = 0;      // kTimeRate*
    u8 flags = 0;     // kTimeFlag* bits
};

/// Reliable, on boundary crossing (360 wrap / 90 / 285). Advisory: the phase
/// in TimeSync already encodes the boundary; the reliable event exists so
/// local one-shot behavior (temp-bit clears, future per-player forms) fires
/// exactly once.
struct TimeEventMsg {
    u8 eventId = 0;  // TimeEventId
    u8 pad = 0;
    f32 time = 0.0f;  // phase at the event
    u16 day = 0;      // day at the event
};

/// Reliable, on mode change + after every stage change (weather is fully
/// reset per stage, 04 §5.6). Carries the current intensity; clients re-pin
/// on receipt and re-run the vanilla dice ramp toward the per-mode target
/// between receipts (04 §4.3 — do not stream the ±1-3/frame ramp).
struct WeatherChangeMsg {
    u8 mode = 0;       // WeatherMode
    u8 thunder = 0;    // mThunderEff.mMode
    u16 intensity = 0; // current raincnt / mSnowCount
    u8 colpat = 0;     // mColpatWeather
    u8 pad = 0;
};

/// M4 room-owner assignment (docs/design/network.md §6). Reliable,
/// host->all, one per changed room. The host is the only emitter: it tracks
/// every player's (stage, room) from the PlayerState/PlayerEvent stream,
/// maintains the sticky ownership table (host defaults to owning its own
/// room; otherwise the first player in the room; transfers only on
/// leave/disconnect), broadcasts each change, and sends the full map to each
/// joiner. Message destination = authority: CombatIntent routes to the
/// room's owner; EnemySnapshot fan-out is scoped to the sender's room.
struct RoomOwnershipMsg {
    char stage[kMaxStageNameLength] = {};  // e.g. "F_SP108"
    s8 room = -1;
    u8 owner = kInvalidPlayerId;  // PlayerId owning this room, or
                                  // kInvalidPlayerId = ownerless
    u8 reserved[2] = {};
};

// ---------------------------------------------------------------------------
// ByteWriter / ByteReader: fixed little-endian primitives, bounds-checked,
// no allocation.
// ---------------------------------------------------------------------------

class ByteWriter {
public:
    ByteWriter(u8* data, u16 capacity) : data_(data), capacity_(capacity) {}

    [[nodiscard]] u16 size() const { return pos_; }
    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] u8* data() { return data_; }

    bool WriteBytes(const void* src, u16 len) {
        if (pos_ + len > capacity_) {
            ok_ = false;
            return false;
        }
        if (len > 0) {
            std::memcpy(data_ + pos_, src, len);
            pos_ += len;
        }
        return true;
    }
    bool WriteU8(u8 v) { return WriteBytes(&v, 1); }
    bool WriteS8(s8 v) { return WriteBytes(&v, 1); }
    bool WriteU16(u16 v) {
        u8 b[2] = {static_cast<u8>(v & 0xFF), static_cast<u8>((v >> 8) & 0xFF)};
        return WriteBytes(b, 2);
    }
    bool WriteS16(s16 v) { return WriteU16(static_cast<u16>(v)); }
    bool WriteU32(u32 v) {
        u8 b[4] = {static_cast<u8>(v & 0xFF), static_cast<u8>((v >> 8) & 0xFF),
            static_cast<u8>((v >> 16) & 0xFF), static_cast<u8>((v >> 24) & 0xFF)};
        return WriteBytes(b, 4);
    }
    bool WriteF32(f32 v) {
        static_assert(sizeof(f32) == 4, "f32 must be 32-bit");
        u32 bits;
        std::memcpy(&bits, &v, 4);
        return WriteU32(bits);
    }
    bool WriteVec3f(const Vec3f& v) {
        return WriteF32(v.x) && WriteF32(v.y) && WriteF32(v.z);
    }
    bool WriteVec3s16(const Vec3s16& v) {
        return WriteS16(v.x) && WriteS16(v.y) && WriteS16(v.z);
    }
    /// Writes exactly `capacity` bytes: the string (truncated to capacity-1)
    /// followed by NUL padding. Always leaves the field NUL-terminated.
    bool WriteFixedString(const char* s, u8 capacity) {
        if (capacity == 0) {
            return false;
        }
        const u8 len = static_cast<u8>(std::strlen(s ? s : ""));
        const u8 written = len < capacity ? len : capacity - 1;
        if (!WriteBytes(s ? s : "", written)) {
            return false;
        }
        u8 pad[kMaxStageNameLength > kMaxNameLength ? kMaxStageNameLength : kMaxNameLength] = {};
        const u16 remaining = static_cast<u16>(capacity - written);
        return WriteBytes(pad, remaining);
    }

private:
    u8* data_;
    u16 capacity_;
    u16 pos_ = 0;
    bool ok_ = true;
};

class ByteReader {
public:
    ByteReader(const u8* data, u16 len) : data_(data), len_(len) {}

    [[nodiscard]] u16 remaining() const { return len_ - pos_; }
    [[nodiscard]] bool ok() const { return ok_; }

    bool ReadBytes(void* dst, u16 len) {
        if (pos_ + len > len_) {
            ok_ = false;
            return false;
        }
        if (len > 0) {
            std::memcpy(dst, data_ + pos_, len);
            pos_ += len;
        }
        return true;
    }
    bool ReadU8(u8& v) { return ReadBytes(&v, 1); }
    bool ReadS8(s8& v) { return ReadBytes(&v, 1); }
    bool ReadU16(u16& v) {
        u8 b[2];
        if (!ReadBytes(b, 2)) {
            return false;
        }
        v = static_cast<u16>(b[0] | (b[1] << 8));
        return true;
    }
    bool ReadS16(s16& v) {
        u16 u;
        if (!ReadU16(u)) {
            return false;
        }
        v = static_cast<s16>(u);
        return true;
    }
    bool ReadU32(u32& v) {
        u8 b[4];
        if (!ReadBytes(b, 4)) {
            return false;
        }
        v = static_cast<u32>(b[0]) | (static_cast<u32>(b[1]) << 8) |
            (static_cast<u32>(b[2]) << 16) | (static_cast<u32>(b[3]) << 24);
        return true;
    }
    bool ReadF32(f32& v) {
        static_assert(sizeof(f32) == 4, "f32 must be 32-bit");
        u32 bits;
        if (!ReadU32(bits)) {
            return false;
        }
        std::memcpy(&v, &bits, 4);
        return true;
    }
    bool ReadVec3f(Vec3f& v) { return ReadF32(v.x) && ReadF32(v.y) && ReadF32(v.z); }
    bool ReadVec3s16(Vec3s16& v) { return ReadS16(v.x) && ReadS16(v.y) && ReadS16(v.z); }
    /// Reads exactly `capacity` bytes into `out` and forces a NUL terminator
    /// at capacity-1.
    bool ReadFixedString(char* out, u8 capacity) {
        if (capacity == 0) {
            return false;
        }
        if (!ReadBytes(out, capacity)) {
            return false;
        }
        out[capacity - 1] = '\0';
        return true;
    }

private:
    const u8* data_;
    u16 len_;
    u16 pos_ = 0;
    bool ok_ = true;
};

// ---------------------------------------------------------------------------
// Message envelope: u16 type + u16 payloadSize + payload
// ---------------------------------------------------------------------------

/// Tagged union of every payload struct. All members are trivially copyable.
///
/// The default ctor zeroes the whole union: a partially-populated payload
/// (e.g. only `version` set on a JoinRequest) still serializes deterministic
/// bytes — the reserved/padding fields can never leak stack garbage onto the
/// wire (review M0 deepseek M3). Construction sites still write `= {}` for
/// clarity.
union PayloadUnion {
    JoinRequestMsg joinRequest;
    JoinAcceptMsg joinAccept;
    JoinRejectMsg joinReject;
    PlayerLeaveMsg playerLeave;
    SessionEndMsg sessionEnd;
    WorldInitMsg worldInit;
    PlayerStateMsg playerState;
    PlayerEventMsg playerEvent;
    EnemySnapshotMsg enemySnapshot;
    EnemyEventMsg enemyEvent;
    CombatIntentMsg combatIntent;
    CombatResultMsg combatResult;
    TimeSyncMsg timeSync;
    TimeEventMsg timeEvent;
    WeatherChangeMsg weatherChange;
    RoomOwnershipMsg roomOwnership;

    PayloadUnion() { std::memset(this, 0, sizeof(PayloadUnion)); }
};

struct Message {
    MsgType type = MsgType::JoinRequest;
    PayloadUnion payload;
};

/// Expected payload size in bytes for each message type (fixed).
u16 WireSize(MsgType type);

/// ENet channel for a message type (00-network.md §1).
constexpr u8 ChannelFor(MsgType type) {
    switch (type) {
    case MsgType::PlayerState:
    case MsgType::EnemySnapshot:
    case MsgType::TimeSync:
        return kChannelUnreliable;
    default:
        return kChannelReliable;
    }
}

/// Serializes `msg` (header + payload) into `w`. Returns false on any
/// bounds/type failure; the writer is left in an unspecified but safe state.
bool SerializeMessage(const Message& msg, ByteWriter& w);

/// Number of wire bytes for a PlayerState payload (header + scale + face +
/// pos + baseTR + full kMaxJoints table).
constexpr u16 PlayerStateWireSize() {
    return 5 + kMaxStageNameLength + (kMaxJoints + 7) / 8 + 10 + 1 + 12 + sizeof(Mtx) +
           kMaxJoints * sizeof(Mtx);
}

/// Parses a header + payload from `r`, validating the type and exact payload
/// size. Returns false if the buffer is malformed or truncated.
bool DeserializeMessage(ByteReader& r, Message& out);

}  // namespace dusk::net
