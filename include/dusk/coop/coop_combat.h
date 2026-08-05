#pragma once

/**
 * \file coop_combat.h
 * M2 — combat validation + hit injection (03-enemies.md §4,
 * m2-design-notes.md §1).
 *
 * Client side: the dCcS::SetAtTgGObjInf intercept (d_cc_s.cpp, TARGET_PC)
 * calls noteAtTgHit() when a local Link's attack contacts a registered enemy
 * puppet; the raw attack fields (atp, AtType bits, powerType, hitType,
 * computed deterministic power, hitPos, attacker pos) are sent to the sim
 * owner as a reliable CombatIntent. Puppet HP is never touched locally (the
 * freeze guarantees the puppet never reaches its damage handler).
 *
 * Host side (sim owner): onCombatIntentHost() validates (entity exists,
 * attacker is a real player, no friendly-fire, in range) and queues intents
 * per enemy per frame; flushHostIntents() (before the actor phase) injects
 * the strongest intent via the enemy's OWN damage collider — the Tg hit flag
 * is set with a synthetic full dCcD_Obj whose dCcD_GObjInf is populated, so
 * the enemy's next execute runs its authentic damage reaction (damage,
 * hitstun, death). The result is broadcast as CombatResult.
 */

#include "dolphin/types.h"
#include "dusk/net/protocol.h"

class fopAc_ac_c;
class cCcD_Obj;
class cXyz;

namespace dusk::coop::combat {

/// Client-side intent capture (d_cc_s.cpp SetAtTgGObjInf, TARGET_PC).
void noteAtTgHit(fopAc_ac_c* atActor, fopAc_ac_c* tgActor, cCcD_Obj* atObj,
                 const cXyz* hitPos);

/// Host-side intent handling (routed from coop::OnGameMessage).
void onGameMessage(net::MsgType type, const net::PayloadUnion& payload);

/// Host-side: injects the frame-batched intents (strongest per enemy) before
/// the actor phase; called from enemy::onGameFrame.
void flushHostIntents();

/// Per-frame reset of the frame-batched capture state.
void beginFrame();

}  // namespace dusk::coop::combat
