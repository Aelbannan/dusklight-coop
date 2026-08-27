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
 * DeserializeMessage stays exact-size by design: the buffer after the header
 * must be exactly the payload (trailing junk is rejected). Channel 0 is reliable (control/events), channel 1 is
 * unreliable sequenced (snapshots — PlayerState),
 * per 00-network.md §1.
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
/// layouts to the absolute-phase contract of 04-time-weather.md §4, and
/// JoinAccept/WorldInit carried TimeStateInfo/WeatherStateInfo. Removed in v9.
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
/// move, 0 = cross-stage / not a SceneChange). Semantic change to an existing
/// field, NO layout change — hence no bump there.
///
/// v7 (M5.1, the parallel-worlds pivot): BREAKING DELETION. The M2/M3/M4
/// authority stack is gone: CombatIntentMsg, CombatResultMsg and
/// RoomOwnershipMsg are deleted, EnemySnapshotMsg is renamed GhostSnapshotMsg
/// (reserved, never consumed), EnemyEventMsg is trimmed to Died-only, and the
/// per-player room-ownership map that rode PlayerState/PlayerEvent is gone
/// (the session no longer sniffs rooms; puppets are whole-session
/// star-relayed). Type count 16 -> 13.
///
/// v8: TimeSync carries the host stage (24 B) so stay-put clients can veto a
/// clock that raced ahead of PlayerState. DeserializeMessage is exact-size.
///
/// v9: BREAKING DELETION of time/weather sync and ghost spectating.
/// Parallel-worlds co-op keeps each save's own clock, sky, and enemies;
/// TimeSync/TimeEvent/WeatherChange, GhostSnapshot, and EnemyEvent are gone,
/// as are TimeStateInfo/WeatherStateInfo on JoinAccept/WorldInit. Type count
/// 13 -> 8. Wire semantics of every surviving message unchanged.
///
/// v10: PlayerEventMsg gained a 16-B stage name so SceneChange can name the
/// destination on the reliable channel (cross-stage no longer depends on a
/// 30-frame unreliable PlayerState burst). Equip/Form/Attention leave it
/// zeroed. Layout change → bump.
///
/// v11: HorseState (unreliable, type 9) — ridden-only visual Epona puppet.
/// Sent alongside PlayerState while checkHorseRide(); receivers spawn a
/// frozen daHorse_c that never occupies mPlayerPtr[1]. Type count 8 -> 9.
/// kPlayerStateFlagHorseRide (bit 5) marks horse vs boar/canoe riding.
constexpr u16 kProtocolVersion = 11;

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
    HorseState = 9,  // v11: ridden Epona pose (unreliable snapshot)
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
    Mount = 1,           // (reserved; horse entity channel is M6)
    Dismount = 2,        // (reserved)
    Respawn = 3,         // (reserved)
    SceneChange = 4,     // data: roomNo
    Equip = 5,           // data: equipItem u16 | selectItemId u8 | clothes u8;
                         //       data2: leftItemJnt u16 | rightItemJnt u16;
                         //       scene: sword item id; reserved: shield item id
                         //       (scene/reserved unused by Equip until this;
                         //       no layout change, no version bump)
    AttentionChange = 6, // data: session entity id of the lock target
                         //       (kInvalidPlayerId/0xFFFF = none or local-only)
};

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
/// size by contract: stage + roster. M1's snapshot-on-join is a burst of
/// ordinary per-frame PlayerState messages sent right after WorldInit — no
/// count-prefixed full-state sections live here. v9 dropped the M3 clock/sky
/// fields; each save keeps its own time and weather.
struct WorldInitMsg {
    StageInfo stage;
    std::array<PlayerInfo, kMaxLocalPlayers> roster;
};

/// PlayerState semantic bits (02-player-state.md §2.1).
constexpr u8 kPlayerStateFlagRiding = 1 << 0;        // mRideStatus != 0
constexpr u8 kPlayerStateFlagInvuln = 1 << 1;        // mDamageTimer > 0
constexpr u8 kPlayerStateFlagSubjectivity = 1 << 2;  // mProcID == PROC_SUBJECTIVITY
constexpr u8 kPlayerStateFlagDowned = 1 << 3;        // downed/dead local life state
constexpr u8 kPlayerStateFlagDemo = 1 << 4;          // mDemo.getDemoType() != 0
constexpr u8 kPlayerStateFlagHorseRide = 1 << 5;     // checkHorseRide() — v11 horse puppet

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

/// Per-frame ridden-Epona pose (v11). Same raw-matrix contract as PlayerState
/// so the puppet horse renders the sender's baked IK/neck/tail without
/// replaying daHorse_c's action procs. Only sent while checkHorseRide();
/// jointCount is 38 on the Horse model (fits kMaxJoints).
struct HorseStateMsg {
    u8 playerId = kInvalidPlayerId;
    s8 roomNo = 0;
    char stage[kMaxStageNameLength] = {};
    u8 jointCount = 0;         // 0..kMaxJoints
    u8 scaleFlags[(kMaxJoints + 7) / 8] = {};
    s16 yaw = 0;               // shape_angle.y
    u8 reserved = 0;
    Vec3f pos;
    Mtx baseTR;
    Mtx joints[kMaxJoints];
};

struct PlayerEventMsg {
    u8 playerId = kInvalidPlayerId;
    u8 eventId = 0;  // PlayerEventId
    /// SceneChange (M4.5 review MINOR 1): 1 = the move is WITHIN the current
    /// stage (same stage, new room), 0 = the stage itself changed
    /// (cross-stage) or not a SceneChange. The host's room-ownership sniff is
    /// gone with the ownership table — the byte now only feeds the
    /// receive-side puppet gate in coop.cpp (same-stage room adoption), which
    /// is valid for same-stage moves only.
    /// Equip: sword item id (dItemNo_*); 0 = field unused (pre-appearance peers).
    u8 scene = 0;
    u8 reserved = 0;  // Equip: shield item id (dItemNo_*); else unused
    u32 data = 0;   // event-specific payload (see PlayerEventId)
    u32 data2 = 0;  // extended event payload (M1: item joints)
    /// v10: SceneChange destination stage (NUL-padded). Empty on other events.
    /// Cross-stage receivers adopt this instead of blanking the name and
    /// waiting on an unreliable PlayerState.
    char stage[kMaxStageNameLength] = {};
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
    HorseStateMsg horseState;

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
    case MsgType::HorseState:
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

/// HorseState payload: id+room+stage+jointCount+scale+yaw+reserved+pos+baseTR+joints.
constexpr u16 HorseStateWireSize() {
    return 1 + 1 + kMaxStageNameLength + 1 + (kMaxJoints + 7) / 8 + 2 + 1 + 12 + sizeof(Mtx) +
           kMaxJoints * sizeof(Mtx);
}

/// Parses a header + payload from `r`, validating the type and exact payload
/// size. Returns false if the buffer is malformed or truncated.
bool DeserializeMessage(ByteReader& r, Message& out);

}  // namespace dusk::net
