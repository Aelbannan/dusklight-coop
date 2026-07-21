#include "dusk/coop/coop_combat.h"
#include "dusk/coop/coop_combat_bridge.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_accessors.h"
#include "dusk/coop/coop_bottles.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_input.h"
#include "dusk/coop/coop_inventory.h"

#include "SSystem/SComponent/c_cc_d.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <unordered_map>
#include <vector>

namespace dusk::coop::combat {

namespace {

std::unordered_map<fpc_ProcID, CombatOwner> g_owners;
std::vector<HitEvent> g_frameHits;
std::vector<SameFrameVictimResult> g_frameResults;

}  // namespace

// Cut-type override must be visible to peekCutTypeOverride / C bridge.
static bool g_cutOverrideActive = false;
static u16 g_cutOverrideValue = 0;
static int g_cutOverrideDepth = 0;
static bool g_suppressPlusDmg = false;

namespace {

HitReactionStrength reactionFromAtp(u8 atp) {
    if (atp == 0) {
        return HitReactionStrength::None;
    }
    if (atp >= 4) {
        return HitReactionStrength::Heavy;
    }
    if (atp >= 2) {
        return HitReactionStrength::Medium;
    }
    return HitReactionStrength::Light;
}

int reactionRank(HitReactionStrength r) { return static_cast<int>(r); }

u16 rawCutType(fopAc_ac_c* actor) {
    if (actor == nullptr) {
        return 0;
    }
    if (fopAcM_GetName(actor) == fpcNm_ALINK_e) {
        return static_cast<daPy_py_c*>(actor)->mCutType;
    }
    const fpc_ProcID pid = fopAcM_GetID(actor);
    auto it = g_owners.find(pid);
    if (it != g_owners.end()) {
        return it->second.cutType;
    }
    return 0;
}

void syncJoinedOwners() {
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (!isJoined(i)) {
            continue;
        }
        fopAc_ac_c* actor = getPlayerActor(i);
        if (actor == nullptr) {
            continue;
        }
        const fpc_ProcID pid = fopAcM_GetID(actor);
        if (pid == fpcM_ERROR_PROCESS_ID_e) {
            continue;
        }
        CombatOwner& owner = g_owners[pid];
        owner.actor = pid;
        owner.faction = Faction::Player;
        owner.player = i;
        owner.cutType = rawCutType(actor);
    }
}

void resolveSameFrameHits() {
    g_frameResults.clear();
    for (const HitEvent& hit : g_frameHits) {
        SameFrameVictimResult* slot = nullptr;
        for (auto& r : g_frameResults) {
            if (r.victimActor == hit.victimActor) {
                slot = &r;
                break;
            }
        }
        if (slot == nullptr) {
            SameFrameVictimResult created{};
            created.victimActor = hit.victimActor;
            created.strongest = hit;
            created.totalDamage = hit.damage;
            created.hitCount = 1;
            g_frameResults.push_back(created);
            continue;
        }
        slot->totalDamage = static_cast<s16>(slot->totalDamage + hit.damage);
        slot->hitCount = static_cast<u8>(slot->hitCount + 1);
        if (reactionRank(hit.reaction) > reactionRank(slot->strongest.reaction)) {
            slot->strongest = hit;
        }
    }
}

}  // namespace

u8 peekCutTypeOverride(u8 nativeCutType) {
    if (!isEnabled() || !g_cutOverrideActive) {
        return nativeCutType;
    }
    return static_cast<u8>(g_cutOverrideValue);
}

bool consumeSuppressPlusDmg() {
    const bool suppress = g_suppressPlusDmg;
    g_suppressPlusDmg = false;
    return suppress;
}

void noteAtTgHit(fopAc_ac_c* atActor, fopAc_ac_c* tgActor, cCcD_Obj* atObj) {
    if (!isEnabled() || atActor == nullptr || tgActor == nullptr) {
        return;
    }
    HitEvent ev{};
    if (const auto atPlayer = playerIdForActor(atActor)) {
        ev.attacker = *atPlayer;
    }
    ev.attackerActor = fopAcM_GetID(atActor);
    ev.victimActor = fopAcM_GetID(tgActor);
    ev.cutType = rawCutType(atActor);
    ev.damage = atObj != nullptr ? static_cast<s16>(atObj->GetAtAtp()) : 0;
    ev.reaction = reactionFromAt(atObj);
    ev.playerVsPlayer =
        playerIdForActor(atActor).has_value() && playerIdForActor(tgActor).has_value();
    registerHit(ev);
}

void init() {
    g_owners.clear();
    g_frameHits.clear();
    g_frameResults.clear();
    g_cutOverrideActive = false;
    g_cutOverrideValue = 0;
    g_cutOverrideDepth = 0;
    g_suppressPlusDmg = false;
}

void reset() { init(); }

void beginFrame() {
    g_cutOverrideActive = false;
    g_cutOverrideValue = 0;
    g_cutOverrideDepth = 0;
    if (isEnabled()) {
        syncJoinedOwners();
    }
}

void endFrame() {
    if (isEnabled()) {
        resolveSameFrameHits();
    }
    g_frameHits.clear();
    g_cutOverrideActive = false;
    g_cutOverrideValue = 0;
    g_cutOverrideDepth = 0;
}

void registerOwner(fpc_ProcID actor, Faction faction, std::optional<PlayerId> player,
                   std::optional<fpc_ProcID> sourceActor) {
    if (actor == fpcM_ERROR_PROCESS_ID_e) {
        return;
    }
    CombatOwner& o = g_owners[actor];
    o.actor = actor;
    o.faction = faction;
    o.player = player;
    o.sourceActor = sourceActor;
}

void unregisterOwner(fpc_ProcID actor) { g_owners.erase(actor); }

void registerPlayerActor(PlayerId id, fopAc_ac_c* actor) {
    if (!isValidPlayer(id) || actor == nullptr) {
        return;
    }
    const fpc_ProcID pid = fopAcM_GetID(actor);
    if (pid == fpcM_ERROR_PROCESS_ID_e) {
        return;
    }
    registerOwner(pid, Faction::Player, id);
    g_owners[pid].cutType = rawCutType(actor);
}

void unregisterPlayerActor(PlayerId id) {
    if (!isValidPlayer(id)) {
        return;
    }
    fopAc_ac_c* actor = getPlayerActor(id);
    if (actor != nullptr) {
        unregisterOwner(fopAcM_GetID(actor));
    }
}

void setOwnerCutType(fpc_ProcID actor, u16 cutType) {
    auto it = g_owners.find(actor);
    if (it != g_owners.end()) {
        it->second.cutType = cutType;
    }
}

const CombatOwner* lookupOwner(fpc_ProcID actor) {
    auto it = g_owners.find(actor);
    return it != g_owners.end() ? &it->second : nullptr;
}

std::optional<PlayerId> playerIdForProc(fpc_ProcID proc) {
    if (proc == fpcM_ERROR_PROCESS_ID_e) {
        return std::nullopt;
    }
    if (const CombatOwner* o = lookupOwner(proc)) {
        if (o->player.has_value()) {
            return o->player;
        }
        if (o->sourceActor.has_value()) {
            return playerIdForProc(*o->sourceActor);
        }
    }
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (!isJoined(i)) {
            continue;
        }
        fopAc_ac_c* actor = getPlayerActor(i);
        if (actor != nullptr && fopAcM_GetID(actor) == proc) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<PlayerId> playerIdForActor(fopAc_ac_c* actor) {
    if (actor == nullptr) {
        return std::nullopt;
    }
    return playerIdForProc(fopAcM_GetID(actor));
}

bool isPlayerFactionActor(fopAc_ac_c* actor) {
    if (actor == nullptr) {
        return false;
    }
    const s16 name = fopAcM_GetName(actor);
    if (name == fpcNm_ALINK_e) {
        return true;
    }
    if (playerIdForActor(actor).has_value()) {
        return true;
    }
    if (const CombatOwner* o = lookupOwner(fopAcM_GetID(actor))) {
        return o->faction == Faction::Player || o->player.has_value();
    }
    return false;
}

u16 cutTypeForActor(fopAc_ac_c* actor) { return rawCutType(actor); }

u16 resolvedCutType() {
    if (g_cutOverrideActive) {
        return g_cutOverrideValue;
    }
    return rawCutType(getPlayerActor(0));
}

void pushAttackCutType(u16 cutType) {
    g_cutOverrideDepth++;
    g_cutOverrideActive = true;
    g_cutOverrideValue = cutType;
}

void popAttackCutType() {
    if (g_cutOverrideDepth > 0) {
        g_cutOverrideDepth--;
    }
    if (g_cutOverrideDepth == 0) {
        g_cutOverrideActive = false;
        g_cutOverrideValue = 0;
    }
}

HitReactionStrength reactionFromAt(cCcD_Obj* atObj) {
    if (atObj == nullptr) {
        return HitReactionStrength::None;
    }
    return reactionFromAtp(atObj->GetAtAtp());
}

bool registerHit(const HitEvent& hit) {
    if (!isEnabled()) {
        return false;
    }
    g_frameHits.push_back(hit);
    return true;
}

const SameFrameVictimResult* strongestHitForVictim(fpc_ProcID victim) {
    for (const auto& r : g_frameResults) {
        if (r.victimActor == victim) {
            return &r;
        }
    }
    return nullptr;
}

bool shouldBlockAtTgCompletely(fopAc_ac_c* atActor, fopAc_ac_c* tgActor) {
    if (!isEnabled() || atActor == nullptr || tgActor == nullptr) {
        return false;
    }
    const auto atPlayer = playerIdForActor(atActor);
    const auto tgPlayer = playerIdForActor(tgActor);
    if (!atPlayer.has_value() || !tgPlayer.has_value()) {
        return false;
    }
    return evaluateFriendlyFire(*atPlayer, *tgPlayer) == FriendlyFireDecision::BlockCompletely;
}

bool filterAtTgHit(fopAc_ac_c* atActor, fopAc_ac_c* tgActor, bool* outContactOnly) {
    if (outContactOnly != nullptr) {
        *outContactOnly = false;
    }
    g_suppressPlusDmg = false;
    if (!isEnabled() || atActor == nullptr || tgActor == nullptr) {
        return true;
    }

    const auto atPlayer = playerIdForActor(atActor);
    const auto tgPlayer = playerIdForActor(tgActor);

    // Player attacking non-player: attribute cut type for subsequent enemy reads.
    if (atPlayer.has_value() && !tgPlayer.has_value()) {
        const u16 cut = rawCutType(atActor);
        pushAttackCutType(cut);
        setOwnerCutType(fopAcM_GetID(atActor), cut);
        return true;
    }

    if (!atPlayer.has_value() || !tgPlayer.has_value()) {
        return true;
    }

    const FriendlyFireDecision decision = evaluateFriendlyFire(*atPlayer, *tgPlayer);
    if (decision == FriendlyFireDecision::BlockCompletely) {
        return false;
    }
    if (decision == FriendlyFireDecision::ContactOnly) {
        g_suppressPlusDmg = true;
        if (outContactOnly != nullptr) {
            *outContactOnly = true;
        }
    }
    return true;
}

FriendlyFireDecision evaluateFriendlyFire(PlayerId attacker, PlayerId victim) {
    if (attacker == victim) {
        return FriendlyFireDecision::AllowFull;
    }
    switch (runtime().friendlyFire) {
    case FriendlyFireMode::Ignore:
        return FriendlyFireDecision::BlockCompletely;
    case FriendlyFireMode::ContactNoDamage:
        return FriendlyFireDecision::ContactOnly;
    case FriendlyFireMode::Full:
        return FriendlyFireDecision::AllowFull;
    }
    return FriendlyFireDecision::BlockCompletely;
}

bool shouldApplyFriendlyFire(PlayerId attacker, PlayerId victim) {
    return evaluateFriendlyFire(attacker, victim) == FriendlyFireDecision::AllowFull;
}

void applyPlayerDamage(PlayerId id, s16 damage) {
    if (!isValidPlayer(id) || damage <= 0) {
        return;
    }
    auto* rt = playerRuntime(id);
    if (rt == nullptr || rt->lifeState != PlayerLifeState::Alive) {
        return;
    }

    inventory::setLife(id, static_cast<s16>(inventory::getLife(id) - damage));
    onPlayerDamaged(id, damage);

    if (inventory::getLife(id) > 0) {
        return;
    }

    if (tryConsumeFairy(id)) {
        inventory::setLife(id, inventory::resources(id).maxLife);
        rt->lifeState = PlayerLifeState::Alive;
        debug::logInfo("combat: P%u revived via fairy", id);
        return;
    }

    markPlayerDowned(id);
    triggerGameOverIfNeeded();
}

void onPlayerDamaged(PlayerId id, s16 /*rawDamage*/) {
    if (!isJoined(id)) {
        return;
    }
    input::rumble(id, 0.55f, 0.85f, 180);
    if (auto* rt = playerRuntime(id)) {
        if (rt->combat.invulnerabilityFrames < 1) {
            rt->combat.invulnerabilityFrames = 1;
        }
    }
}

bool tryConsumeFairy(PlayerId id) {
    const u8 slots = bottles::unlockedSlotCount();
    for (u8 s = 0; s < slots; ++s) {
        if (bottles::getContents(id, s) == dItemNo_FAIRY_e) {
            return bottles::tryConsume(id, s);
        }
    }
    return false;
}

bool allPlayersDowned() {
    if (!isEnabled()) {
        return false;
    }
    bool any = false;
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (!isJoined(i)) {
            continue;
        }
        any = true;
        if (runtime().playerRuntime[i].lifeState == PlayerLifeState::Alive) {
            return false;
        }
    }
    return any;
}

void markPlayerDowned(PlayerId id) {
    auto* rt = playerRuntime(id);
    if (rt == nullptr) {
        return;
    }
    rt->lifeState = PlayerLifeState::Downed;
    inventory::setLife(id, 0);
    debug::logInfo("combat: P%u downed", id);
}

void triggerGameOverIfNeeded() {
    if (!allPlayersDowned()) {
        return;
    }
    fopAc_ac_c* p0 = getPlayerActor(0);
    if (p0 != nullptr && fopAcM_GetName(p0) == fpcNm_ALINK_e) {
        static_cast<daPy_py_c*>(p0)->onForceGameOver();
        debug::logInfo("combat: all players downed — forcing game over");
    }
}

bool shouldSuppressGameOver(PlayerId id) {
    if (!isEnabled() || !isJoined(id)) {
        return false;
    }
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (i == id || !isJoined(i)) {
            continue;
        }
        if (runtime().playerRuntime[i].lifeState == PlayerLifeState::Alive) {
            return true;
        }
    }
    return false;
}

}  // namespace dusk::coop::combat

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
extern "C" u8 dusk_coop_overrideCutType(u8 nativeCutType) {
    return dusk::coop::combat::peekCutTypeOverride(nativeCutType);
}
#endif
