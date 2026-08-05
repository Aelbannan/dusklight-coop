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

bool SerializeTime(const TimeInfo& t, ByteWriter& w) {
    return w.WriteU32(t.phase) && w.WriteU32(t.elapsedMs);
}

bool DeserializeTime(TimeInfo& t, ByteReader& r) {
    return r.ReadU32(t.phase) && r.ReadU32(t.elapsedMs);
}

bool SerializeWeather(const WeatherInfo& wth, ByteWriter& w) {
    return w.WriteU8(wth.id) && w.WriteU8(wth.intensity) && w.WriteU16(wth.reserved);
}

bool DeserializeWeather(WeatherInfo& wth, ByteReader& r) {
    return r.ReadU8(wth.id) && r.ReadU8(wth.intensity) && r.ReadU16(wth.reserved);
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
           SerializeStage(m.stage, w) && SerializeTime(m.time, w) &&
           SerializeWeather(m.weather, w) && SerializeRoster(m.roster, w);
}

bool DeserializeJoinAccept(JoinAcceptMsg& m, ByteReader& r) {
    return r.ReadU8(m.assignedPlayerId) && r.ReadBytes(m.reserved, 3) &&
           DeserializeStage(m.stage, r) && DeserializeTime(m.time, r) &&
           DeserializeWeather(m.weather, r) && DeserializeRoster(m.roster, r);
}

bool SerializeWorldInit(const WorldInitMsg& m, ByteWriter& w) {
    return SerializeStage(m.stage, w) && w.WriteU16(m.playerStateCount) &&
           w.WriteU16(m.enemyStateCount) && SerializeRoster(m.roster, w);
}

bool DeserializeWorldInit(WorldInitMsg& m, ByteReader& r) {
    return DeserializeStage(m.stage, r) && r.ReadU16(m.playerStateCount) &&
           r.ReadU16(m.enemyStateCount) && DeserializeRoster(m.roster, r);
}

bool SerializePlayerState(const PlayerStateMsg& m, ByteWriter& w) {
    if (!w.WriteU8(m.playerId) || !w.WriteU8(m.scene) || !w.WriteU8(m.form) ||
        !w.WriteU8(m.movementFlags) || !w.WriteU8(m.jointCount) ||
        !w.WriteBytes(m.cosmetics, 3) || !w.WriteS8(m.itemAction) ||
        !w.WriteU8(m.invincibility) || !w.WriteU8(m.reserved) || !w.WriteU32(m.stateFlags) ||
        !w.WriteVec3f(m.pos) || !w.WriteVec3s16(m.rot) || !w.WriteVec3s16(m.upperLimbRot))
    {
        return false;
    }
    for (const auto& j : m.joints) {
        if (!w.WriteVec3s16(j)) {
            return false;
        }
    }
    return true;
}

bool DeserializePlayerState(PlayerStateMsg& m, ByteReader& r) {
    if (!r.ReadU8(m.playerId) || !r.ReadU8(m.scene) || !r.ReadU8(m.form) ||
        !r.ReadU8(m.movementFlags) || !r.ReadU8(m.jointCount) || !r.ReadBytes(m.cosmetics, 3) ||
        !r.ReadS8(m.itemAction) || !r.ReadU8(m.invincibility) || !r.ReadU8(m.reserved) ||
        !r.ReadU32(m.stateFlags) || !r.ReadVec3f(m.pos) || !r.ReadVec3s16(m.rot) ||
        !r.ReadVec3s16(m.upperLimbRot))
    {
        return false;
    }
    for (auto& j : m.joints) {
        if (!r.ReadVec3s16(j)) {
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
        return 1 + 3 + 20 + 8 + 4 + kMaxLocalPlayers * 36;  // 324
    case MsgType::JoinReject:
    case MsgType::PlayerLeave:
    case MsgType::SessionEnd:
        return 4;
    case MsgType::WorldInit:
        return 20 + 2 + 2 + kMaxLocalPlayers * 36;  // 312
    case MsgType::PlayerState:
        return 11 + 4 + 12 + 6 + 6 + kMaxJoints * 6;  // 279
    case MsgType::PlayerEvent:
        return 8;
    case MsgType::EnemySnapshot:
        return 2 + 2 + 2 + 2 + 1 + 1 + 2 + 4 + 12;  // 28
    case MsgType::EnemyEvent:
        return 2 + 2 + 1 + 1;  // 6
    case MsgType::CombatIntent:
        return 4 + 2 + 2 + 4 + 12;  // 24
    case MsgType::CombatResult:
        return 2 + 2 + 2 + 1 + 1 + 4;  // 12
    case MsgType::TimeSync:
        return 8;
    case MsgType::TimeEvent:
        return 1 + 3 + 4;  // 8
    case MsgType::WeatherChange:
        return 1 + 1 + 2;  // 4
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
        return w.WriteU8(msg.payload.playerEvent.playerId) && w.WriteU8(msg.payload.playerEvent.eventId) &&
               w.WriteU8(msg.payload.playerEvent.scene) && w.WriteU8(msg.payload.playerEvent.reserved) &&
               w.WriteU32(msg.payload.playerEvent.data);
    case MsgType::EnemySnapshot:
        return w.WriteU16(msg.payload.enemySnapshot.enemyId) &&
               w.WriteU16(msg.payload.enemySnapshot.type) && w.WriteU16(msg.payload.enemySnapshot.hp) &&
               w.WriteU16(msg.payload.enemySnapshot.maxHp) &&
               w.WriteU8(msg.payload.enemySnapshot.aggro) && w.WriteU8(msg.payload.enemySnapshot.flags) &&
               w.WriteS16(msg.payload.enemySnapshot.angle) && w.WriteU32(msg.payload.enemySnapshot.anim) &&
               w.WriteVec3f(msg.payload.enemySnapshot.pos);
    case MsgType::EnemyEvent:
        return w.WriteU16(msg.payload.enemyEvent.enemyId) && w.WriteU16(msg.payload.enemyEvent.data) &&
               w.WriteU8(msg.payload.enemyEvent.eventId) && w.WriteU8(msg.payload.enemyEvent.flags);
    case MsgType::CombatIntent:
        return w.WriteU8(msg.payload.combatIntent.attackerId) &&
               w.WriteU8(msg.payload.combatIntent.attackKind) &&
               w.WriteU8(msg.payload.combatIntent.targetPlayerId) &&
               w.WriteU8(msg.payload.combatIntent.reserved) &&
               w.WriteU16(msg.payload.combatIntent.targetEnemyId) &&
               w.WriteU16(msg.payload.combatIntent.damage) && w.WriteU32(msg.payload.combatIntent.seq) &&
               w.WriteVec3f(msg.payload.combatIntent.position);
    case MsgType::CombatResult:
        return w.WriteU16(msg.payload.combatResult.targetEnemyId) &&
               w.WriteU16(msg.payload.combatResult.damage) &&
               w.WriteU16(msg.payload.combatResult.newHp) &&
               w.WriteU8(msg.payload.combatResult.outcome) &&
               w.WriteU8(msg.payload.combatResult.attackerId) &&
               w.WriteU32(msg.payload.combatResult.seq);
    case MsgType::TimeSync:
        return w.WriteU32(msg.payload.timeSync.phase) && w.WriteU32(msg.payload.timeSync.elapsedMs);
    case MsgType::TimeEvent:
        return w.WriteU8(msg.payload.timeEvent.eventId) &&
               w.WriteBytes(msg.payload.timeEvent.reserved, 3) &&
               w.WriteU32(msg.payload.timeEvent.timePhase);
    case MsgType::WeatherChange:
        return w.WriteU8(msg.payload.weatherChange.weatherId) &&
               w.WriteU8(msg.payload.weatherChange.intensity) &&
               w.WriteU16(msg.payload.weatherChange.reserved);
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
               r.ReadU8(out.payload.playerEvent.eventId) && r.ReadU8(out.payload.playerEvent.scene) &&
               r.ReadU8(out.payload.playerEvent.reserved) && r.ReadU32(out.payload.playerEvent.data);
    case MsgType::EnemySnapshot:
        return r.ReadU16(out.payload.enemySnapshot.enemyId) &&
               r.ReadU16(out.payload.enemySnapshot.type) &&
               r.ReadU16(out.payload.enemySnapshot.hp) &&
               r.ReadU16(out.payload.enemySnapshot.maxHp) &&
               r.ReadU8(out.payload.enemySnapshot.aggro) &&
               r.ReadU8(out.payload.enemySnapshot.flags) &&
               r.ReadS16(out.payload.enemySnapshot.angle) &&
               r.ReadU32(out.payload.enemySnapshot.anim) &&
               r.ReadVec3f(out.payload.enemySnapshot.pos);
    case MsgType::EnemyEvent:
        return r.ReadU16(out.payload.enemyEvent.enemyId) &&
               r.ReadU16(out.payload.enemyEvent.data) &&
               r.ReadU8(out.payload.enemyEvent.eventId) &&
               r.ReadU8(out.payload.enemyEvent.flags);
    case MsgType::CombatIntent:
        return r.ReadU8(out.payload.combatIntent.attackerId) &&
               r.ReadU8(out.payload.combatIntent.attackKind) &&
               r.ReadU8(out.payload.combatIntent.targetPlayerId) &&
               r.ReadU8(out.payload.combatIntent.reserved) &&
               r.ReadU16(out.payload.combatIntent.targetEnemyId) &&
               r.ReadU16(out.payload.combatIntent.damage) &&
               r.ReadU32(out.payload.combatIntent.seq) &&
               r.ReadVec3f(out.payload.combatIntent.position);
    case MsgType::CombatResult:
        return r.ReadU16(out.payload.combatResult.targetEnemyId) &&
               r.ReadU16(out.payload.combatResult.damage) &&
               r.ReadU16(out.payload.combatResult.newHp) &&
               r.ReadU8(out.payload.combatResult.outcome) &&
               r.ReadU8(out.payload.combatResult.attackerId) &&
               r.ReadU32(out.payload.combatResult.seq);
    case MsgType::TimeSync:
        return r.ReadU32(out.payload.timeSync.phase) &&
               r.ReadU32(out.payload.timeSync.elapsedMs);
    case MsgType::TimeEvent:
        return r.ReadU8(out.payload.timeEvent.eventId) &&
               r.ReadBytes(out.payload.timeEvent.reserved, 3) &&
               r.ReadU32(out.payload.timeEvent.timePhase);
    case MsgType::WeatherChange:
        return r.ReadU8(out.payload.weatherChange.weatherId) &&
               r.ReadU8(out.payload.weatherChange.intensity) &&
               r.ReadU16(out.payload.weatherChange.reserved);
    }
    return false;
}

}  // namespace dusk::net
