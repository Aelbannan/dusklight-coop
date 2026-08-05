#include "dusk/net/protocol.h"

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

bool SerializeTimeState(const TimeStateInfo& t, ByteWriter& w) {
    return w.WriteF32(t.time) && w.WriteU16(t.day) && w.WriteU8(t.rate) && w.WriteU8(t.flags);
}

bool DeserializeTimeState(TimeStateInfo& t, ByteReader& r) {
    return r.ReadF32(t.time) && r.ReadU16(t.day) && r.ReadU8(t.rate) && r.ReadU8(t.flags);
}

bool SerializeWeatherState(const WeatherStateInfo& wth, ByteWriter& w) {
    return w.WriteU8(wth.mode) && w.WriteU8(wth.thunder) && w.WriteU16(wth.intensity) &&
           w.WriteU8(wth.colpat) && w.WriteU8(wth.pad);
}

bool DeserializeWeatherState(WeatherStateInfo& wth, ByteReader& r) {
    return r.ReadU8(wth.mode) && r.ReadU8(wth.thunder) && r.ReadU16(wth.intensity) &&
           r.ReadU8(wth.colpat) && r.ReadU8(wth.pad);
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
           SerializeStage(m.stage, w) && SerializeTimeState(m.time, w) &&
           SerializeWeatherState(m.weather, w) && SerializeRoster(m.roster, w);
}

bool DeserializeJoinAccept(JoinAcceptMsg& m, ByteReader& r) {
    return r.ReadU8(m.assignedPlayerId) && r.ReadBytes(m.reserved, 3) &&
           DeserializeStage(m.stage, r) && DeserializeTimeState(m.time, r) &&
           DeserializeWeatherState(m.weather, r) && DeserializeRoster(m.roster, r);
}

bool SerializeWorldInit(const WorldInitMsg& m, ByteWriter& w) {
    return SerializeStage(m.stage, w) && SerializeTimeState(m.time, w) &&
           SerializeWeatherState(m.weather, w) && SerializeRoster(m.roster, w);
}

bool DeserializeWorldInit(WorldInitMsg& m, ByteReader& r) {
    return DeserializeStage(m.stage, r) && DeserializeTimeState(m.time, r) &&
           DeserializeWeatherState(m.weather, r) && DeserializeRoster(m.roster, r);
}

bool SerializePlayerState(const PlayerStateMsg& m, ByteWriter& w) {
    if (!w.WriteU8(m.playerId) || !w.WriteS8(m.roomNo) || !w.WriteU8(m.form) ||
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

bool DeserializePlayerState(PlayerStateMsg& m, ByteReader& r) {
    if (!r.ReadU8(m.playerId) || !r.ReadS8(m.roomNo) || !r.ReadU8(m.form) ||
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
        // assignedPlayerId + reserved + stage(20) + time(8) + weather(6) + roster
        return 1 + 3 + 20 + 8 + 6 + kMaxLocalPlayers * 36;  // 326
    case MsgType::JoinReject:
    case MsgType::PlayerLeave:
    case MsgType::SessionEnd:
        return 4;
    case MsgType::WorldInit:
        // stage(20) + time(8) + weather(6) + roster (M3 v5: time+weather added)
        return 20 + 8 + 6 + kMaxLocalPlayers * 36;  // 322
    case MsgType::PlayerState:
        // raw-matrix pose (Rev 3 D4): 5 (id/room/form/flags/jointCount)
        // + 5 scaleFlags + 10 (yaw/pitch face bck/btp/frame/reserved)
        // + 12 pos + sizeof(Mtx) baseTR + 40*sizeof(Mtx) joints = 2001
        // with TP's 3x4 Mtx (48 B). See PlayerStateWireSize(); reviewed m1
        // MINOR m1 corrected the stale 2657 (4x4 Mtx) figure.
        return PlayerStateWireSize();
    case MsgType::PlayerEvent:
        return 4 + 4 + 4;  // 12
    case MsgType::EnemySnapshot:
        // enemyId,type,hp,maxHp,aggro,flags,angle,anim,pos,speed,semantics,reserved[3]
        // = 2+2+2+2+1+1+2+4+12+12+1+3 = 44
        return 2 + 2 + 2 + 2 + 1 + 1 + 2 + 4 + 12 + 12 + 1 + 3;  // 44
    case MsgType::EnemyEvent:
        return 2 + 2 + 1 + 1 + 1 + 3;  // 10 (flagMask + reserved)
    case MsgType::CombatIntent:
        // attackerId,powerType,hitType,targetPlayerId,targetEnemyId,atp,reserved,
        // computedPower,seq,atType,hitPos,attackerPos
        // = 1+1+1+1+2+1+1+2+4+4+12+12 = 42
        return 1 + 1 + 1 + 1 + 2 + 1 + 1 + 2 + 4 + 4 + 12 + 12;  // 42
    case MsgType::CombatResult:
        return 2 + 2 + 2 + 1 + 1 + 4;  // 12
    case MsgType::TimeSync:
        // f32 time + u16 day + u8 rate + u8 flags
        return 4 + 2 + 1 + 1;  // 8
    case MsgType::TimeEvent:
        // u8 eventId + u8 pad + f32 time + u16 day
        return 1 + 1 + 4 + 2;  // 8
    case MsgType::WeatherChange:
        // u8 mode + u8 thunder + u16 intensity + u8 colpat + u8 pad
        return 1 + 1 + 2 + 1 + 1;  // 6
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
               w.WriteU32(msg.payload.playerEvent.data) && w.WriteU32(msg.payload.playerEvent.data2);
    case MsgType::EnemySnapshot:
        return w.WriteU16(msg.payload.enemySnapshot.enemyId) &&
               w.WriteU16(msg.payload.enemySnapshot.type) && w.WriteU16(msg.payload.enemySnapshot.hp) &&
               w.WriteU16(msg.payload.enemySnapshot.maxHp) &&
               w.WriteU8(msg.payload.enemySnapshot.aggro) && w.WriteU8(msg.payload.enemySnapshot.flags) &&
               w.WriteS16(msg.payload.enemySnapshot.angle) && w.WriteU32(msg.payload.enemySnapshot.anim) &&
               w.WriteVec3f(msg.payload.enemySnapshot.pos) &&
               w.WriteVec3f(msg.payload.enemySnapshot.speed) &&
               w.WriteU8(msg.payload.enemySnapshot.semantics) &&
               w.WriteBytes(msg.payload.enemySnapshot.reserved, 3);
    case MsgType::EnemyEvent:
        return w.WriteU16(msg.payload.enemyEvent.enemyId) && w.WriteU16(msg.payload.enemyEvent.data) &&
               w.WriteU8(msg.payload.enemyEvent.eventId) && w.WriteU8(msg.payload.enemyEvent.flags) &&
               w.WriteU8(msg.payload.enemyEvent.flagMask) &&
               w.WriteBytes(msg.payload.enemyEvent.reserved, 3);
    case MsgType::CombatIntent:
        return w.WriteU8(msg.payload.combatIntent.attackerId) &&
               w.WriteU8(msg.payload.combatIntent.powerType) &&
               w.WriteU8(msg.payload.combatIntent.hitType) &&
               w.WriteU8(msg.payload.combatIntent.targetPlayerId) &&
               w.WriteU16(msg.payload.combatIntent.targetEnemyId) &&
               w.WriteU8(msg.payload.combatIntent.atp) &&
               w.WriteU8(msg.payload.combatIntent.reserved) &&
               w.WriteU16(msg.payload.combatIntent.computedPower) &&
               w.WriteU32(msg.payload.combatIntent.seq) &&
               w.WriteU32(msg.payload.combatIntent.atType) &&
               w.WriteVec3f(msg.payload.combatIntent.hitPos) &&
               w.WriteVec3f(msg.payload.combatIntent.attackerPos);
    case MsgType::CombatResult:
        return w.WriteU16(msg.payload.combatResult.targetEnemyId) &&
               w.WriteU16(msg.payload.combatResult.damage) &&
               w.WriteU16(msg.payload.combatResult.newHp) &&
               w.WriteU8(msg.payload.combatResult.outcome) &&
               w.WriteU8(msg.payload.combatResult.attackerId) &&
               w.WriteU32(msg.payload.combatResult.seq);
    case MsgType::TimeSync:
        return w.WriteF32(msg.payload.timeSync.time) && w.WriteU16(msg.payload.timeSync.day) &&
               w.WriteU8(msg.payload.timeSync.rate) && w.WriteU8(msg.payload.timeSync.flags);
    case MsgType::TimeEvent:
        return w.WriteU8(msg.payload.timeEvent.eventId) && w.WriteU8(msg.payload.timeEvent.pad) &&
               w.WriteF32(msg.payload.timeEvent.time) && w.WriteU16(msg.payload.timeEvent.day);
    case MsgType::WeatherChange:
        return w.WriteU8(msg.payload.weatherChange.mode) &&
               w.WriteU8(msg.payload.weatherChange.thunder) &&
               w.WriteU16(msg.payload.weatherChange.intensity) &&
               w.WriteU8(msg.payload.weatherChange.colpat) && w.WriteU8(msg.payload.weatherChange.pad);
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
        typeRaw > static_cast<u16>(MsgType::WeatherChange))
    {
        return false;
    }
    const auto type = static_cast<MsgType>(typeRaw);
    if (payloadSize != WireSize(type) || r.remaining() < payloadSize) {
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
               r.ReadU32(out.payload.playerEvent.data2);
    case MsgType::EnemySnapshot:
        return r.ReadU16(out.payload.enemySnapshot.enemyId) &&
               r.ReadU16(out.payload.enemySnapshot.type) &&
               r.ReadU16(out.payload.enemySnapshot.hp) &&
               r.ReadU16(out.payload.enemySnapshot.maxHp) &&
               r.ReadU8(out.payload.enemySnapshot.aggro) &&
               r.ReadU8(out.payload.enemySnapshot.flags) &&
               r.ReadS16(out.payload.enemySnapshot.angle) &&
               r.ReadU32(out.payload.enemySnapshot.anim) &&
               r.ReadVec3f(out.payload.enemySnapshot.pos) &&
               r.ReadVec3f(out.payload.enemySnapshot.speed) &&
               r.ReadU8(out.payload.enemySnapshot.semantics) &&
               r.ReadBytes(out.payload.enemySnapshot.reserved, 3);
    case MsgType::EnemyEvent:
        return r.ReadU16(out.payload.enemyEvent.enemyId) &&
               r.ReadU16(out.payload.enemyEvent.data) &&
               r.ReadU8(out.payload.enemyEvent.eventId) &&
               r.ReadU8(out.payload.enemyEvent.flags) &&
               r.ReadU8(out.payload.enemyEvent.flagMask) &&
               r.ReadBytes(out.payload.enemyEvent.reserved, 3);
    case MsgType::CombatIntent:
        return r.ReadU8(out.payload.combatIntent.attackerId) &&
               r.ReadU8(out.payload.combatIntent.powerType) &&
               r.ReadU8(out.payload.combatIntent.hitType) &&
               r.ReadU8(out.payload.combatIntent.targetPlayerId) &&
               r.ReadU16(out.payload.combatIntent.targetEnemyId) &&
               r.ReadU8(out.payload.combatIntent.atp) &&
               r.ReadU8(out.payload.combatIntent.reserved) &&
               r.ReadU16(out.payload.combatIntent.computedPower) &&
               r.ReadU32(out.payload.combatIntent.seq) &&
               r.ReadU32(out.payload.combatIntent.atType) &&
               r.ReadVec3f(out.payload.combatIntent.hitPos) &&
               r.ReadVec3f(out.payload.combatIntent.attackerPos);
    case MsgType::CombatResult:
        return r.ReadU16(out.payload.combatResult.targetEnemyId) &&
               r.ReadU16(out.payload.combatResult.damage) &&
               r.ReadU16(out.payload.combatResult.newHp) &&
               r.ReadU8(out.payload.combatResult.outcome) &&
               r.ReadU8(out.payload.combatResult.attackerId) &&
               r.ReadU32(out.payload.combatResult.seq);
    case MsgType::TimeSync:
        return r.ReadF32(out.payload.timeSync.time) && r.ReadU16(out.payload.timeSync.day) &&
               r.ReadU8(out.payload.timeSync.rate) && r.ReadU8(out.payload.timeSync.flags);
    case MsgType::TimeEvent:
        return r.ReadU8(out.payload.timeEvent.eventId) && r.ReadU8(out.payload.timeEvent.pad) &&
               r.ReadF32(out.payload.timeEvent.time) && r.ReadU16(out.payload.timeEvent.day);
    case MsgType::WeatherChange:
        return r.ReadU8(out.payload.weatherChange.mode) &&
               r.ReadU8(out.payload.weatherChange.thunder) &&
               r.ReadU16(out.payload.weatherChange.intensity) &&
               r.ReadU8(out.payload.weatherChange.colpat) && r.ReadU8(out.payload.weatherChange.pad);
    }
    return false;
}

}  // namespace dusk::net
