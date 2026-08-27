#include "dusk/net/protocol.h"

#include <cmath>

namespace dusk::net {

namespace {

// ---------------------------------------------------------------------------
// Per-type fixed payload serializers
// ---------------------------------------------------------------------------

bool SerializePlayerInfo(const PlayerInfo& p, ByteWriter& w) {
    return w.WriteU8(p.playerId) && w.WriteU8(p.present) && w.WriteU8(p.state) &&
           w.WriteU8(p.reserved) && w.WriteFixedString(p.name, kMaxNameLength);
}

bool DeserializePlayerInfo(PlayerInfo& p, ByteReader& r) {
    return r.ReadU8(p.playerId) && r.ReadU8(p.present) && r.ReadU8(p.state) &&
           r.ReadU8(p.reserved) && r.ReadFixedString(p.name, kMaxNameLength);
}

bool SerializeRoster(const std::array<PlayerInfo, kMaxLocalPlayers>& roster, ByteWriter& w) {
    for (const auto& entry : roster) {
        if (!SerializePlayerInfo(entry, w)) {
            return false;
        }
    }
    return true;
}

bool DeserializeRoster(std::array<PlayerInfo, kMaxLocalPlayers>& roster, ByteReader& r) {
    for (auto& entry : roster) {
        if (!DeserializePlayerInfo(entry, r)) {
            return false;
        }
    }
    return true;
}

bool SerializeStage(const StageInfo& s, ByteWriter& w) {
    return w.WriteFixedString(s.stage, kMaxStageNameLength) && w.WriteS8(s.room) &&
           w.WriteS8(s.layer) && w.WriteS16(s.point);
}

bool DeserializeStage(StageInfo& s, ByteReader& r) {
    return r.ReadFixedString(s.stage, kMaxStageNameLength) && r.ReadS8(s.room) &&
           r.ReadS8(s.layer) && r.ReadS16(s.point);
}

bool SerializeJoinRequest(const JoinRequestMsg& m, ByteWriter& w) {
    return w.WriteU32(m.version) && w.WriteU8(m.requestedSlot) && w.WriteBytes(m.reserved, 3) &&
           w.WriteFixedString(m.name, kMaxNameLength);
}

bool DeserializeJoinRequest(JoinRequestMsg& m, ByteReader& r) {
    return r.ReadU32(m.version) && r.ReadU8(m.requestedSlot) && r.ReadBytes(m.reserved, 3) &&
           r.ReadFixedString(m.name, kMaxNameLength);
}

bool SerializeJoinAccept(const JoinAcceptMsg& m, ByteWriter& w) {
    return w.WriteU8(m.assignedPlayerId) && w.WriteBytes(m.reserved, 3) &&
           SerializeStage(m.stage, w) && SerializeRoster(m.roster, w);
}

bool DeserializeJoinAccept(JoinAcceptMsg& m, ByteReader& r) {
    return r.ReadU8(m.assignedPlayerId) && r.ReadBytes(m.reserved, 3) &&
           DeserializeStage(m.stage, r) && DeserializeRoster(m.roster, r);
}

bool SerializeWorldInit(const WorldInitMsg& m, ByteWriter& w) {
    return SerializeStage(m.stage, w) && SerializeRoster(m.roster, w);
}

bool DeserializeWorldInit(WorldInitMsg& m, ByteReader& r) {
    return DeserializeStage(m.stage, r) && DeserializeRoster(m.roster, r);
}

bool SerializePlayerState(const PlayerStateMsg& m, ByteWriter& w) {
    if (!w.WriteU8(m.playerId) || !w.WriteS8(m.roomNo) ||
        !w.WriteFixedString(m.stage, kMaxStageNameLength) || !w.WriteU8(m.form) ||
        !w.WriteU8(m.stateFlags) || !w.WriteU8(m.jointCount) ||
        !w.WriteBytes(m.scaleFlags, sizeof(m.scaleFlags)) || !w.WriteS16(m.yaw) ||
        !w.WriteS16(m.pitch) || !w.WriteU16(m.faceBckIdx) || !w.WriteU16(m.faceBtpIdx) ||
        !w.WriteS16(m.faceFrame) || !w.WriteU8(m.reserved) || !w.WriteVec3f(m.pos) ||
        !w.WriteBytes(m.baseTR, sizeof(Mtx)))
    {
        return false;
    }
    for (const auto& j : m.joints) {
        if (!w.WriteBytes(j, sizeof(Mtx))) {
            return false;
        }
    }
    return true;
}

bool FiniteF32(f32 v) {
    return std::isfinite(v);
}

bool FiniteVec3(const Vec3f& v) {
    return FiniteF32(v.x) && FiniteF32(v.y) && FiniteF32(v.z);
}

bool FiniteMtx(const Mtx& m) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (!FiniteF32(m[r][c])) {
                return false;
            }
        }
    }
    return true;
}

bool DeserializePlayerState(PlayerStateMsg& m, ByteReader& r) {
    if (!r.ReadU8(m.playerId) || !r.ReadS8(m.roomNo) ||
        !r.ReadFixedString(m.stage, kMaxStageNameLength) || !r.ReadU8(m.form) ||
        !r.ReadU8(m.stateFlags) || !r.ReadU8(m.jointCount) ||
        !r.ReadBytes(m.scaleFlags, sizeof(m.scaleFlags)) || !r.ReadS16(m.yaw) ||
        !r.ReadS16(m.pitch) || !r.ReadU16(m.faceBckIdx) || !r.ReadU16(m.faceBtpIdx) ||
        !r.ReadS16(m.faceFrame) || !r.ReadU8(m.reserved) || !r.ReadVec3f(m.pos) ||
        !r.ReadBytes(m.baseTR, sizeof(Mtx)))
    {
        return false;
    }
    for (auto& j : m.joints) {
        if (!r.ReadBytes(j, sizeof(Mtx))) {
            return false;
        }
    }
    // Semantic validation (review M0 deepseek M5): a jointCount beyond the
    // fixed table would make M1's apply code index out of bounds. Reject the
    // whole packet at parse rather than trusting the value.
    if (m.jointCount > kMaxJoints) {
        return false;
    }
    if (!FiniteVec3(m.pos) || !FiniteMtx(m.baseTR)) {
        return false;
    }
    for (u8 j = 0; j < m.jointCount; ++j) {
        if (!FiniteMtx(m.joints[j])) {
            return false;
        }
    }
    return true;
}

bool SerializeHorseState(const HorseStateMsg& m, ByteWriter& w) {
    if (!w.WriteU8(m.playerId) || !w.WriteS8(m.roomNo) ||
        !w.WriteFixedString(m.stage, kMaxStageNameLength) || !w.WriteU8(m.jointCount) ||
        !w.WriteBytes(m.scaleFlags, sizeof(m.scaleFlags)) || !w.WriteS16(m.yaw) ||
        !w.WriteU8(m.reserved) || !w.WriteVec3f(m.pos) || !w.WriteBytes(m.baseTR, sizeof(Mtx)))
    {
        return false;
    }
    for (const auto& j : m.joints) {
        if (!w.WriteBytes(j, sizeof(Mtx))) {
            return false;
        }
    }
    return true;
}

bool DeserializeHorseState(HorseStateMsg& m, ByteReader& r) {
    if (!r.ReadU8(m.playerId) || !r.ReadS8(m.roomNo) ||
        !r.ReadFixedString(m.stage, kMaxStageNameLength) || !r.ReadU8(m.jointCount) ||
        !r.ReadBytes(m.scaleFlags, sizeof(m.scaleFlags)) || !r.ReadS16(m.yaw) ||
        !r.ReadU8(m.reserved) || !r.ReadVec3f(m.pos) || !r.ReadBytes(m.baseTR, sizeof(Mtx)))
    {
        return false;
    }
    for (auto& j : m.joints) {
        if (!r.ReadBytes(j, sizeof(Mtx))) {
            return false;
        }
    }
    if (m.jointCount > kMaxJoints) {
        return false;
    }
    if (!FiniteVec3(m.pos) || !FiniteMtx(m.baseTR)) {
        return false;
    }
    for (u8 j = 0; j < m.jointCount; ++j) {
        if (!FiniteMtx(m.joints[j])) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Message-level API
// ---------------------------------------------------------------------------

u16 WireSize(MsgType type) {
    switch (type) {
    case MsgType::JoinRequest:
        return 4 + 1 + 3 + kMaxNameLength;  // 40
    case MsgType::JoinAccept:
        // assignedPlayerId + reserved + stage(20) + roster (v9: no time/weather)
        return 1 + 3 + 20 + kMaxLocalPlayers * 36;  // 312
    case MsgType::JoinReject:
    case MsgType::PlayerLeave:
    case MsgType::SessionEnd:
        return 4;
    case MsgType::WorldInit:
        // stage(20) + roster (v9: no time/weather)
        return 20 + kMaxLocalPlayers * 36;  // 308
    case MsgType::PlayerState:
        // raw-matrix pose (Rev 3 D4): 5 (id/room/form/flags/jointCount)
        // + 16 stage + 5 scaleFlags + 10 (yaw/pitch face bck/btp/frame/
        // reserved) + 12 pos + sizeof(Mtx) baseTR + 40*sizeof(Mtx) joints
        // = 2017 with TP's 3x4 Mtx (48 B). See PlayerStateWireSize();
        // capstone MINOR I corrected the stale 2001 (the 16-byte stage
        // field was omitted from the old figure).
        return PlayerStateWireSize();
    case MsgType::PlayerEvent:
        // id/event/scene/reserved + data + data2 + stage (v10)
        return 4 + 4 + 4 + kMaxStageNameLength;  // 28
    case MsgType::HorseState:
        return HorseStateWireSize();
    }
    return 0;
}

bool SerializeMessage(const Message& msg, ByteWriter& w) {
    const u16 payloadSize = WireSize(msg.type);
    if (payloadSize == 0) {
        return false;
    }
    // Envelope: u16 type + u16 payloadSize, little-endian.
    if (!w.WriteU16(static_cast<u16>(msg.type)) || !w.WriteU16(payloadSize)) {
        return false;
    }
    switch (msg.type) {
    case MsgType::JoinRequest:
        return SerializeJoinRequest(msg.payload.joinRequest, w);
    case MsgType::JoinAccept:
        return SerializeJoinAccept(msg.payload.joinAccept, w);
    case MsgType::JoinReject:
        return w.WriteU8(msg.payload.joinReject.reason) && w.WriteBytes(msg.payload.joinReject.reserved, 3);
    case MsgType::PlayerLeave:
        return w.WriteU8(msg.payload.playerLeave.playerId) && w.WriteBytes(msg.payload.playerLeave.reserved, 3);
    case MsgType::SessionEnd:
        return w.WriteU8(msg.payload.sessionEnd.reason) && w.WriteBytes(msg.payload.sessionEnd.reserved, 3);
    case MsgType::WorldInit:
        return SerializeWorldInit(msg.payload.worldInit, w);
    case MsgType::PlayerState:
        return SerializePlayerState(msg.payload.playerState, w);
    case MsgType::PlayerEvent:
        return w.WriteU8(msg.payload.playerEvent.playerId) &&
               w.WriteU8(msg.payload.playerEvent.eventId) &&
               w.WriteU8(msg.payload.playerEvent.scene) &&
               w.WriteU8(msg.payload.playerEvent.reserved) &&
               w.WriteU32(msg.payload.playerEvent.data) &&
               w.WriteU32(msg.payload.playerEvent.data2) &&
               w.WriteFixedString(msg.payload.playerEvent.stage, kMaxStageNameLength);
    case MsgType::HorseState:
        return SerializeHorseState(msg.payload.horseState, w);
    }
    return false;
}

bool DeserializeMessage(ByteReader& r, Message& out) {
    u16 typeRaw = 0;
    u16 payloadSize = 0;
    if (!r.ReadU16(typeRaw) || !r.ReadU16(payloadSize)) {
        return false;
    }
    if (typeRaw < static_cast<u16>(MsgType::JoinRequest) ||
        typeRaw > static_cast<u16>(MsgType::HorseState))
    {
        // v11: 9 types 1..9. EnemyEvent(10), TimeSync(11)/TimeEvent(12)/
        // WeatherChange(13), and the older CombatIntent/CombatResult/
        // RoomOwnership ids are out of range. Id 9 is HorseState (the old
        // GhostSnapshot layout is rejected by exact-size).
        return false;
    }
    const auto type = static_cast<MsgType>(typeRaw);
    if (payloadSize != WireSize(type) || r.remaining() != payloadSize) {
        return false;
    }
    out.type = type;
    switch (type) {
    case MsgType::JoinRequest:
        return DeserializeJoinRequest(out.payload.joinRequest, r);
    case MsgType::JoinAccept:
        return DeserializeJoinAccept(out.payload.joinAccept, r);
    case MsgType::JoinReject:
        return r.ReadU8(out.payload.joinReject.reason) && r.ReadBytes(out.payload.joinReject.reserved, 3);
    case MsgType::PlayerLeave:
        return r.ReadU8(out.payload.playerLeave.playerId) &&
               r.ReadBytes(out.payload.playerLeave.reserved, 3);
    case MsgType::SessionEnd:
        return r.ReadU8(out.payload.sessionEnd.reason) &&
               r.ReadBytes(out.payload.sessionEnd.reserved, 3);
    case MsgType::WorldInit:
        return DeserializeWorldInit(out.payload.worldInit, r);
    case MsgType::PlayerState:
        return DeserializePlayerState(out.payload.playerState, r);
    case MsgType::PlayerEvent:
        return r.ReadU8(out.payload.playerEvent.playerId) &&
               r.ReadU8(out.payload.playerEvent.eventId) &&
               r.ReadU8(out.payload.playerEvent.scene) &&
               r.ReadU8(out.payload.playerEvent.reserved) &&
               r.ReadU32(out.payload.playerEvent.data) &&
               r.ReadU32(out.payload.playerEvent.data2) &&
               r.ReadFixedString(out.payload.playerEvent.stage, kMaxStageNameLength);
    case MsgType::HorseState:
        return DeserializeHorseState(out.payload.horseState, r);
    }
    return false;
}

}  // namespace dusk::net
