#pragma once

#include "dusk/coop/coop_types.h"

#include <optional>

class fopAc_ac_c;
class cCcD_Obj;

namespace dusk::coop::combat {

enum class Faction : uint8_t {
    Player,
    Enemy,
    Neutral,
};

enum class HitReactionStrength : uint8_t {
    None,
    Light,
    Medium,
    Heavy,
};

enum class FriendlyFireDecision : uint8_t {
    BlockCompletely,  // Ignore — no contact
    ContactOnly,      // ContactNoDamage — callbacks ok, no PlusDmg
    AllowFull,        // Full damage
};

struct CombatOwner {
    fpc_ProcID actor = fpcM_ERROR_PROCESS_ID_e;
    Faction faction = Faction::Neutral;
    std::optional<PlayerId> player;
    std::optional<fpc_ProcID> sourceActor;
    u16 cutType = 0;
};

struct HitEvent {
    PlayerId attacker = 0;
    fpc_ProcID attackerActor = fpcM_ERROR_PROCESS_ID_e;
    fpc_ProcID victimActor = fpcM_ERROR_PROCESS_ID_e;
    u16 cutType = 0;
    s16 damage = 0;
    HitReactionStrength reaction = HitReactionStrength::None;
    bool fromProjectile = false;
    bool playerVsPlayer = false;
};

struct SameFrameVictimResult {
    fpc_ProcID victimActor = fpcM_ERROR_PROCESS_ID_e;
    HitEvent strongest{};
    s16 totalDamage = 0;
    u8 hitCount = 0;
};

void init();
void reset();

void beginFrame();
void endFrame();

// --- Ownership registry ---
void registerOwner(fpc_ProcID actor, Faction faction, std::optional<PlayerId> player,
                   std::optional<fpc_ProcID> sourceActor = std::nullopt);
void unregisterOwner(fpc_ProcID actor);
void registerPlayerActor(PlayerId id, fopAc_ac_c* actor);
void unregisterPlayerActor(PlayerId id);
void setOwnerCutType(fpc_ProcID actor, u16 cutType);
const CombatOwner* lookupOwner(fpc_ProcID actor);
std::optional<PlayerId> playerIdForActor(fopAc_ac_c* actor);
std::optional<PlayerId> playerIdForProc(fpc_ProcID proc);
bool isPlayerFactionActor(fopAc_ac_c* actor);

// Cut type from attacking Link/proxy (registry), not always global P0.
u16 cutTypeForActor(fopAc_ac_c* actor);
u16 resolvedCutType();
void pushAttackCutType(u16 cutType);
void popAttackCutType();

// --- Hit registration / same-frame ---
HitReactionStrength reactionFromAt(cCcD_Obj* atObj);
bool registerHit(const HitEvent& hit);
const SameFrameVictimResult* strongestHitForVictim(fpc_ProcID victim);

// Called from dCcS before native SetAtTg work. Returns false → skip entire hit (Ignore).
// When contactOnly is set, caller should suppress PlusDmg but keep callbacks.
bool filterAtTgHit(fopAc_ac_c* atActor, fopAc_ac_c* tgActor, bool* outContactOnly);

// Lightweight Ignore-only check for ChkNoHitGAtTg (no cut-type side effects).
bool shouldBlockAtTgCompletely(fopAc_ac_c* atActor, fopAc_ac_c* tgActor);

FriendlyFireDecision evaluateFriendlyFire(PlayerId attacker, PlayerId victim);
bool shouldApplyFriendlyFire(PlayerId attacker, PlayerId victim);

// --- Per-player damage / fairy / game-over ---
void applyPlayerDamage(PlayerId id, s16 damage);
void onPlayerDamaged(PlayerId id, s16 rawDamage);
bool tryConsumeFairy(PlayerId id);
bool allPlayersDowned();
void markPlayerDowned(PlayerId id);
void triggerGameOverIfNeeded();

// True when co-op should suppress the vanilla single-player game-over for this player.
bool shouldSuppressGameOver(PlayerId id);

// Used by dusk_coop_overrideCutType (combat bridge).
u8 peekCutTypeOverride(u8 nativeCutType);

// After filterAtTgHit: when true, SetAtTgGObjInf should skip PlusDmg.
bool consumeSuppressPlusDmg();

// Record a hit with AT collider damage/reaction (call from SetAtTgGObjInf).
void noteAtTgHit(fopAc_ac_c* atActor, fopAc_ac_c* tgActor, cCcD_Obj* atObj);

}  // namespace dusk::coop::combat
