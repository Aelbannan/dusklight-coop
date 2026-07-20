#pragma once

#include "dusk/coop/coop_types.h"
#include "dusk/coop/coop_difficulty.h"

namespace dusk::coop::drops {

enum class DropCategory : uint8_t {
    Healing,
    Ammo,
    Rupees,
    Other,
};

// Integer spawn budget (spent 1:1 per accepted native candidate unit).
struct DropBudget {
    u16 healing = 0;
    u16 ammo = 0;
    u16 rupees = 0;
    u16 other = 0;
};

// Fractional credits (Q16.16) before integerization at encounter begin.
struct DropCredits {
    difficulty::Q16_16 healing = 0;
    difficulty::Q16_16 ammo = 0;
    difficulty::Q16_16 rupees = 0;
};

struct EncounterSnapshot {
    u64 seed = 0;
    u8 partySize = 1;
    difficulty::Profile profile = difficulty::Profile::Normal;
    DropBudget budget{};
    DropCredits credits{};  // pre-integerization record for reproducibility
    u32 originalEnemyCount = 0;

    // Compounded scalars frozen at begin — never shrink on disconnect/down.
    difficulty::Q16_16 countMul = difficulty::ONE;
    difficulty::Q16_16 perEnemyHpMul = difficulty::ONE;
    difficulty::Q16_16 staggerMul = difficulty::ONE;
    difficulty::Q16_16 heartsMul = difficulty::ONE;
    difficulty::Q16_16 ammoMul = difficulty::ONE;
    difficulty::Q16_16 rupeesMul = difficulty::ONE;
    bool active = false;
    bool heartsDisabled = false;
};

void init();
void reset();

// Build a deterministic snapshot from party size + current profile + vanilla room count.
// Same inputs → identical budget/scalars (seed included).
EncounterSnapshot makeSnapshot(u8 partySize, difficulty::Profile profile, u32 originalEnemyCount,
                               u64 seed);

void beginEncounter(const EncounterSnapshot& snap);
void endEncounter();
const EncounterSnapshot& currentEncounter();
bool encounterActive();

// Ensure a room encounter is snapshotted once. Uses joined count at first call.
// Subsequent calls with the same room are no-ops while active.
bool ensureEncounter(u32 originalEnemyCount, u64 seed);

// Strategy A: classify a native enemy-drop candidate and spend encounter credit.
// Returns false → suppress spawn (no credit / hearts disabled / inactive ok→allow when inactive).
bool gateEnemyDropCandidate(u8 itemNo);

DropCategory classifyItem(u8 itemNo);
u16 creditCost(u8 itemNo, DropCategory category);

bool trySpendDropCredit(DropCategory category);
bool trySpendDropCredit(DropCategory category, u16 units);

// Free-for-all race: among acceptors, lower PlayerId wins (documented rule).
bool resolvePickupRace(PlayerId a, PlayerId b, DropCategory category, PlayerId* winnerOut);

// Multi-candidate variant: picks lowest accepting id from [0, count).
bool resolvePickupRace(const PlayerId* candidates, u8 count, DropCategory category,
                       PlayerId* winnerOut);

// Gate E capacity: full life/arrows/bombs/wallet → cannot accept (leave for others).
bool playerCanAccept(PlayerId id, DropCategory category);
bool playerCanAcceptItem(PlayerId id, u8 itemNo);

// Desired clone count from snapshotted count mul (shared budget, not ∝ live clones).
u32 desiredTotalEnemies();
u32 clonesPerEligibleSource();

}  // namespace dusk::coop::drops
