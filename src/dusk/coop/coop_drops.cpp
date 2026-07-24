#include "dusk/coop/coop_drops.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_inventory.h"

#include "d/d_item_data.h"

namespace dusk::coop::drops {
namespace {

EncounterSnapshot g_encounter{};

// Vanilla-expected units per eligible ordinary enemy (Strategy A baseline without parsed tables).
constexpr s32 kVanillaHeartsPerEnemy = 1;
constexpr s32 kVanillaAmmoPerTwoEnemies = 1;  // ≈0.5 bundles/enemy
constexpr s32 kVanillaRupeesPerEnemy = 10;

u16 creditsToBudget(difficulty::Q16_16 credits) {
    // credits is already a Q16 total; round half-up to whole spawn units.
    const s32 rounded = difficulty::mulRoundToInt(credits, 1);
    if (rounded < 0) {
        return 0;
    }
    return static_cast<u16>(rounded > 0xFFFF ? 0xFFFF : rounded);
}

DropBudget budgetFromCredits(const DropCredits& c) {
    DropBudget b;
    b.healing = creditsToBudget(c.healing);
    b.ammo = creditsToBudget(c.ammo);
    b.rupees = creditsToBudget(c.rupees);
    b.other = 0;
    return b;
}

bool canAcceptBombs(PlayerId id) {
    const auto& res = inventory::resources(id);
    for (u8 bag = 0; bag < 3; ++bag) {
        // Any bag below a generous soft max counts as accepting ammo/bombs.
        // Exact bag max is global; Gate E tryAddBombs clamps.
        if (res.bombCounts[bag] < 30) {
            return true;
        }
    }
    return false;
}

}  // namespace

void init() { g_encounter = {}; }
void reset() { init(); }

EncounterSnapshot makeSnapshot(u8 partySize, difficulty::Profile profile, u32 originalEnemyCount,
                               u64 seed) {
    EncounterSnapshot snap;
    snap.seed = seed;
    snap.partySize = partySize < 1 ? 1 : partySize;
    snap.profile = profile;
    snap.originalEnemyCount = originalEnemyCount;
    snap.active = true;

    // Temporarily apply profile scalars for compounding (caller may already have set it).
    const difficulty::ProfileScalars saved = difficulty::scalars();
    const difficulty::Profile savedProfile = difficulty::currentProfile();
    difficulty::setProfile(profile);
    const difficulty::ProfileScalars& ps = difficulty::scalars();

    snap.countMul = difficulty::compoundEnemyCount(snap.partySize, ps);
    // Assume count will be achieved when computing per-enemy HP (example_encounter).
    snap.perEnemyHpMul = difficulty::compoundPerEnemyHp(snap.partySize, snap.countMul, ps);
    snap.staggerMul = difficulty::compoundStagger(snap.partySize, ps);
    snap.heartsMul = difficulty::compoundHearts(snap.partySize, ps);
    snap.ammoMul = difficulty::compoundAmmo(snap.partySize, ps);
    snap.rupeesMul = difficulty::compoundRupees(snap.partySize, ps);
    snap.heartsDisabled = (ps.hearts == difficulty::ZERO);

    const s32 enemies = static_cast<s32>(originalEnemyCount);
    const s32 vanillaHearts = enemies * kVanillaHeartsPerEnemy;
    const s32 vanillaAmmo =
        enemies <= 0 ? 0 : (enemies + 1) / 2 * kVanillaAmmoPerTwoEnemies;  // ceil(n/2)
    const s32 vanillaRupees = enemies * kVanillaRupeesPerEnemy;

    // Integer base × Q16 mul → Q16 total (no >>16).
    snap.credits.healing =
        snap.heartsDisabled
            ? difficulty::ZERO
            : static_cast<difficulty::Q16_16>(static_cast<s64>(vanillaHearts) *
                                              static_cast<s64>(snap.heartsMul));
    snap.credits.ammo = static_cast<difficulty::Q16_16>(static_cast<s64>(vanillaAmmo) *
                                                        static_cast<s64>(snap.ammoMul));
    snap.credits.rupees = static_cast<difficulty::Q16_16>(static_cast<s64>(vanillaRupees) *
                                                          static_cast<s64>(snap.rupeesMul));

    snap.budget = budgetFromCredits(snap.credits);

    difficulty::setProfile(savedProfile);
    (void)saved;

    return snap;
}

void beginEncounter(const EncounterSnapshot& snap) {
    g_encounter = snap;
    g_encounter.active = true;
    debug::logInfo(
        "drops: encounter begin party=%u profile=%u enemies=%u countMul=%.3f hpMul=%.3f "
        "budget H/A/R=%u/%u/%u seed=%llu",
        static_cast<unsigned>(snap.partySize), static_cast<unsigned>(snap.profile),
        snap.originalEnemyCount, difficulty::toFloat(snap.countMul),
        difficulty::toFloat(snap.perEnemyHpMul), snap.budget.healing, snap.budget.ammo,
        snap.budget.rupees, static_cast<unsigned long long>(snap.seed));
}

void endEncounter() { g_encounter = {}; }

const EncounterSnapshot& currentEncounter() { return g_encounter; }

bool encounterActive() { return g_encounter.active; }

bool ensureEncounter(u32 originalEnemyCount, u64 seed) {
    if (g_encounter.active) {
        return false;
    }
    u8 party = runtime().joinedPlayerCount;
    if (party < 1) {
        party = 1;
    }
    // Freeze party size at encounter begin — later disconnect/down must not shrink.
    beginEncounter(makeSnapshot(party, difficulty::currentProfile(), originalEnemyCount, seed));
    return true;
}

DropCategory classifyItem(u8 itemNo) {
    switch (itemNo) {
    case dItemNo_HEART_e:
    case dItemNo_TRIPLE_HEART_e:
    case dItemNo_RECOVERY_FAILY_e:
        return DropCategory::Healing;
    case dItemNo_ARROW_10_e:
    case dItemNo_ARROW_20_e:
    case dItemNo_ARROW_30_e:
    case dItemNo_ARROW_1_e:
    case dItemNo_BOMB_5_e:
    case dItemNo_BOMB_10_e:
    case dItemNo_BOMB_20_e:
    case dItemNo_BOMB_30_e:
    case dItemNo_WATER_BOMB_5_e:
    case dItemNo_WATER_BOMB_10_e:
    case dItemNo_WATER_BOMB_20_e:
    case dItemNo_WATER_BOMB_30_e:
    case dItemNo_PACHINKO_SHOT_e:
        return DropCategory::Ammo;
    case dItemNo_GREEN_RUPEE_e:
    case dItemNo_BLUE_RUPEE_e:
    case dItemNo_YELLOW_RUPEE_e:
    case dItemNo_RED_RUPEE_e:
    case dItemNo_PURPLE_RUPEE_e:
    case dItemNo_ORANGE_RUPEE_e:
    case dItemNo_SILVER_RUPEE_e:
        return DropCategory::Rupees;
    default:
        return DropCategory::Other;
    }
}

u16 creditCost(u8 itemNo, DropCategory category) {
    switch (category) {
    case DropCategory::Healing:
        if (itemNo == dItemNo_TRIPLE_HEART_e) {
            return 3;
        }
        return 1;
    case DropCategory::Ammo:
        if (itemNo == dItemNo_ARROW_20_e || itemNo == dItemNo_BOMB_10_e ||
            itemNo == dItemNo_WATER_BOMB_10_e) {
            return 2;
        }
        if (itemNo == dItemNo_ARROW_30_e || itemNo == dItemNo_BOMB_20_e ||
            itemNo == dItemNo_WATER_BOMB_20_e) {
            return 3;
        }
        if (itemNo == dItemNo_BOMB_30_e || itemNo == dItemNo_WATER_BOMB_30_e) {
            return 3;
        }
        return 1;
    case DropCategory::Rupees:
        switch (itemNo) {
        case dItemNo_GREEN_RUPEE_e:
            return 1;
        case dItemNo_BLUE_RUPEE_e:
            return 5;
        case dItemNo_YELLOW_RUPEE_e:
            return 10;
        case dItemNo_RED_RUPEE_e:
            return 20;
        case dItemNo_PURPLE_RUPEE_e:
            return 50;
        case dItemNo_ORANGE_RUPEE_e:
            return 100;
        case dItemNo_SILVER_RUPEE_e:
            return 200;
        default:
            return 1;
        }
    case DropCategory::Other:
        return 1;
    }
    return 1;
}

bool trySpendDropCredit(DropCategory category) { return trySpendDropCredit(category, 1); }

bool trySpendDropCredit(DropCategory category, u16 units) {
    if (units == 0) {
        return true;
    }
    auto& b = g_encounter.budget;
    switch (category) {
    case DropCategory::Healing:
        if (b.healing < units) {
            return false;
        }
        b.healing = static_cast<u16>(b.healing - units);
        return true;
    case DropCategory::Ammo:
        if (b.ammo < units) {
            return false;
        }
        b.ammo = static_cast<u16>(b.ammo - units);
        return true;
    case DropCategory::Rupees:
        if (b.rupees < units) {
            return false;
        }
        b.rupees = static_cast<u16>(b.rupees - units);
        return true;
    case DropCategory::Other:
        if (b.other < units) {
            return false;
        }
        b.other = static_cast<u16>(b.other - units);
        return true;
    }
    return false;
}

bool gateEnemyDropCandidate(u8 itemNo) {
    // One-player / inactive: leave native behavior (≈ vanilla).
    if (!g_encounter.active) {
        return true;
    }
    // 1-player Normal must remain indistinguishable from vanilla drop rolls.
    if (g_encounter.partySize <= 1 &&
        g_encounter.profile == difficulty::Profile::Normal) {
        return true;
    }
    if (itemNo == dItemNo_NONE_e) {
        return false;
    }

    const DropCategory cat = classifyItem(itemNo);
    if (cat == DropCategory::Healing && g_encounter.heartsDisabled) {
        debug::logInfo("drops: suppress heart (profile disabled)");
        return false;
    }
    if (cat == DropCategory::Other) {
        // Permanent / uncategorized: always allow (not budgeted).
        return true;
    }

    const u16 cost = creditCost(itemNo, cat);
    if (!trySpendDropCredit(cat, cost)) {
        debug::logInfo("drops: suppress item=%u cat=%u (budget exhausted)", itemNo,
                       static_cast<unsigned>(cat));
        return false;
    }
    return true;
}

bool resolvePickupRace(PlayerId a, PlayerId b, DropCategory category, PlayerId* winnerOut) {
    PlayerId pair[2] = {a, b};
    return resolvePickupRace(pair, 2, category, winnerOut);
}

bool resolvePickupRace(const PlayerId* candidates, u8 count, DropCategory category,
                       PlayerId* winnerOut) {
    if (winnerOut == nullptr || candidates == nullptr || count == 0) {
        return false;
    }
    bool found = false;
    PlayerId best = 0;
    for (u8 i = 0; i < count; ++i) {
        const PlayerId id = candidates[i];
        if (!playerCanAccept(id, category)) {
            continue;
        }
        if (!found || id < best) {
            best = id;
            found = true;
        }
    }
    if (!found) {
        return false;
    }
    *winnerOut = best;
    return true;
}

bool playerCanAccept(PlayerId id, DropCategory category) {
    if (!isValidPlayer(id) || !isJoined(id)) {
        return false;
    }
    // Downed players do not collect.
    if (runtime().playerRuntime[id].lifeState != PlayerLifeState::Alive) {
        return false;
    }
    const auto& res = inventory::resources(id);
    switch (category) {
    case DropCategory::Healing:
        return res.life < res.maxLife;
    case DropCategory::Ammo:
        return res.arrows < res.maxArrows || canAcceptBombs(id);
    case DropCategory::Rupees:
        return res.rupees < res.maxRupees;
    case DropCategory::Other:
        return true;
    }
    return false;
}

bool playerCanAcceptItem(PlayerId id, u8 itemNo) {
    const DropCategory cat = classifyItem(itemNo);
    if (!isValidPlayer(id) || !isJoined(id)) {
        return false;
    }
    if (runtime().playerRuntime[id].lifeState != PlayerLifeState::Alive) {
        return false;
    }
    const auto& res = inventory::resources(id);
    switch (itemNo) {
    case dItemNo_HEART_e:
    case dItemNo_TRIPLE_HEART_e:
    case dItemNo_RECOVERY_FAILY_e:
        return res.life < res.maxLife;
    case dItemNo_ARROW_10_e:
    case dItemNo_ARROW_20_e:
    case dItemNo_ARROW_30_e:
    case dItemNo_ARROW_1_e:
        return res.arrows < res.maxArrows;
    case dItemNo_BOMB_5_e:
    case dItemNo_BOMB_10_e:
    case dItemNo_BOMB_20_e:
    case dItemNo_BOMB_30_e:
    case dItemNo_WATER_BOMB_5_e:
    case dItemNo_WATER_BOMB_10_e:
    case dItemNo_WATER_BOMB_20_e:
    case dItemNo_WATER_BOMB_30_e:
        return canAcceptBombs(id);
    case dItemNo_PACHINKO_SHOT_e:
        return res.pachinko < 50;
    case dItemNo_GREEN_RUPEE_e:
    case dItemNo_BLUE_RUPEE_e:
    case dItemNo_YELLOW_RUPEE_e:
    case dItemNo_RED_RUPEE_e:
    case dItemNo_PURPLE_RUPEE_e:
    case dItemNo_ORANGE_RUPEE_e:
    case dItemNo_SILVER_RUPEE_e:
        return res.rupees < res.maxRupees;
    default:
        return playerCanAccept(id, cat);
    }
}

u32 desiredTotalEnemies() {
    if (!g_encounter.active) {
        return g_encounter.originalEnemyCount;
    }
    return difficulty::desiredEnemyCount(g_encounter.originalEnemyCount, g_encounter.countMul);
}

u32 clonesPerEligibleSource() {
    if (!g_encounter.active) {
        return 0;
    }
    if (g_encounter.partySize < 2) {
        return 0;
    }
    const u32 desired = desiredTotalEnemies();
    const u32 original = g_encounter.originalEnemyCount;
    if (desired <= original || original == 0) {
        return 0;
    }
    // Distribute evenly; remainder goes to earlier sources via caller ordinal.
    return (desired - original + original - 1) / original;  // ceil((desired-original)/original)
}

}  // namespace dusk::coop::drops
