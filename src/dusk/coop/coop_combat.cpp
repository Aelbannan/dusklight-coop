#include "dusk/coop/coop_combat.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_enemy.h"

#include "d/d_com_inf_game.h"
#include "d/d_cc_uty.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <aurora/lib/logging.hpp>

#include <unordered_map>
#include <vector>

namespace dusk::coop::combat {

namespace {

aurora::Module CombatLog("dusk::coop::combat");

using net::kInvalidPlayerId;
using net::kMaxLocalPlayers;
using enemy::kInvalidEnemyId;

// Per-enemy frame-batched intents on the sim owner (host).
std::unordered_map<u16, std::vector<net::CombatIntentMsg>> g_hostQueue;
u32 g_seq = 1;

void SendResult(const net::CombatIntentMsg& intent, net::CombatOutcome outcome, u16 damage,
                u16 newHp) {
    net::PayloadUnion payload = {};
    payload.combatResult.targetEnemyId = intent.targetEnemyId;
    payload.combatResult.damage = damage;
    payload.combatResult.newHp = newHp;
    payload.combatResult.outcome = static_cast<u8>(outcome);
    payload.combatResult.attackerId = intent.attackerId;
    payload.combatResult.seq = intent.seq;
    dusk::coop::sendGameMessage(net::MsgType::CombatResult, payload);
}

void RejectIntent(const net::CombatIntentMsg& intent, const char* why) {
    CombatLog.debug("combat: rejected intent seq={} target=0x{:04X} ({})", intent.seq,
        intent.targetEnemyId, why);
    SendResult(intent, net::CombatOutcome::Rejected, 0, 0);
}

}  // namespace

void noteAtTgHit(fopAc_ac_c* atActor, fopAc_ac_c* tgActor, cCcD_Obj* atObj,
                 const cXyz* hitPos) {
    // M4 room ownership: the sim owner applies local hits natively (the
    // collision pass already ran; the enemy's own handler polls the real hit
    // flag). Intents are non-owner -> ROOM owner only. A machine that owns
    // the target's room sims the enemy natively, so its local hits never
    // become intents.
    if (!dusk::coop::sessionActive()) {
        return;
    }
    if (dusk::coop::amIRoomOwner(fopAcM_GetRoomNo(tgActor))) {
        return;
    }
    if (atActor == nullptr || tgActor == nullptr || atObj == nullptr || hitPos == nullptr) {
        return;
    }
    // Only the local real Link's attacks forward intents (a remote puppet on
    // this machine never attacks — puppets are frozen). Slot 0 is the real
    // Link on a client (M1 create neutralization).
    if (atActor != dComIfGp_getPlayer(0)) {
        return;
    }
    if (!dusk::coop::enemy::isRegistered(tgActor)) {
        return;  // not an enemy puppet (local non-whitelisted enemy: local AI)
    }
    const auto* adapter = dusk::coop::enemy::findAdapter(fopAcM_GetName(tgActor));
    if (adapter == nullptr) {
        return;
    }
    const u16 enemyId = dusk::coop::enemy::entityIdForActor(tgActor);
    if (enemyId == kInvalidEnemyId) {
        return;
    }

    net::CombatIntentMsg intent = {};
    intent.attackerId = dusk::coop::selfId();
    intent.targetEnemyId = enemyId;
    intent.atp = atObj->GetAtAtp();
    intent.atType = atObj->GetAtType();
    intent.powerType = adapter->defaultPowerType;
    intent.hitPos.x = hitPos->x;
    intent.hitPos.y = hitPos->y;
    intent.hitPos.z = hitPos->z;
    intent.attackerPos.x = atActor->current.pos.x;
    intent.attackerPos.y = atActor->current.pos.y;
    intent.attackerPos.z = atActor->current.pos.z;
    intent.seq = ++g_seq;

    // Deterministic power + hit type: at_power_check with the REAL At
    // collider (the client's equipment multipliers never enter here — they
    // live in cc_at_check's slot-0 block on the owner, m2-design-notes §1).
    dCcU_AtInfo info = {};
    info.mpCollider = atObj;
    info.mPowerType = adapter->defaultPowerType;
    at_power_check(&info);
    intent.computedPower = info.mAttackPower;
    intent.hitType = info.mHitType;

    net::PayloadUnion payload = {};
    payload.combatIntent = intent;
    if (!dusk::coop::sendGameMessage(net::MsgType::CombatIntent, payload)) {
        CombatLog.debug("combat: intent drop (session not sendable)");
        return;
    }
    CombatLog.debug("combat: intent seq={} enemy=0x{:04X} atp={} atType=0x{:08X} power={}", intent.seq,
        enemyId, intent.atp, intent.atType, intent.computedPower);
}

void onGameMessage(net::MsgType type, const net::PayloadUnion& payload) {
    if (type != net::MsgType::CombatIntent) {
        return;
    }
    if (!dusk::coop::sessionActive()) {
        return;
    }
    const auto& intent = payload.combatIntent;

    // --- validation (00-network.md §7, 03-enemies.md §4.4, M4) ---
    if (intent.attackerId >= kMaxLocalPlayers || !dusk::coop::rosterPresent(intent.attackerId)) {
        RejectIntent(intent, "unknown attacker");
        return;
    }
    if (intent.targetPlayerId != kInvalidPlayerId) {
        RejectIntent(intent, "friendly fire off");
        return;
    }
    if (intent.targetEnemyId == kInvalidEnemyId) {
        RejectIntent(intent, "invalid target");
        return;
    }
    fopAc_ac_c* enemy = dusk::coop::enemy::actorForEntityId(intent.targetEnemyId);
    if (enemy == nullptr || !dusk::coop::enemy::isRegistered(enemy)) {
        RejectIntent(intent, "target gone");
        return;
    }
    // M4 room ownership: only the target's ROOM owner validates + injects.
    // The host routes intents to the owner peer, so a non-owner machine that
    // receives one (a forged/misrouted copy) ignores it rather than fighting
    // the owner's result.
    if (!dusk::coop::amIRoomOwner(fopAcM_GetRoomNo(enemy))) {
        return;
    }
    // Range: the collider contact already implies proximity; the owner's check
    // is a loose sanity bound against forged intents (LAN trust model).
    const f32 dx = intent.attackerPos.x - enemy->current.pos.x;
    const f32 dz = intent.attackerPos.z - enemy->current.pos.z;
    if (dx * dx + dz * dz > 4000.0f * 4000.0f) {
        RejectIntent(intent, "out of range");
        return;
    }

    // Queue per enemy; flushHostIntents() aggregates + injects before the
    // actor phase (per-enemy-per-frame batching, 03-enemies.md §4.4).
    g_hostQueue[intent.targetEnemyId].push_back(intent);
}

void flushHostIntents() {
    if (g_hostQueue.empty()) {
        return;
    }
    for (auto& kv : g_hostQueue) {
        const u16 enemyId = kv.first;
        std::vector<net::CombatIntentMsg>& intents = kv.second;
        fopAc_ac_c* enemy = dusk::coop::enemy::actorForEntityId(enemyId);
        if (enemy == nullptr) {
            // Died between the queue and the flush (e.g. the host's own Link
            // killed it this frame) — nothing to inject; acknowledge the miss.
            SendResult(intents.front(), net::CombatOutcome::Miss, 0, 0);
            continue;
        }
        // Aggregate: the strongest reaction (highest atp) is injected; the
        // enemy's own handler then runs its authentic damage/death — vanilla
        // itself is lossy for same-frame multi-hits on one enemy (a single Tg
        // hit flag), so no sum model is needed (see m2-design-notes §1).
        const net::CombatIntentMsg* strongest = &intents.front();
        for (const auto& i : intents) {
            if (i.atp > strongest->atp) {
                strongest = &i;
            }
        }
        dusk::coop::enemy::applyInjectedHit(enemy, *strongest);
        // The CombatResult (accurate post-execute HP) is sent by
        // hostOnExecuted via the pending-hit latch; nothing more to do here.
    }
    g_hostQueue.clear();
}

}  // namespace dusk::coop::combat
