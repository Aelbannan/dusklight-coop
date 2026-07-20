#pragma once

#include "dusk/coop/coop_types.h"

namespace dusk::coop::difficulty {

enum class Profile : uint8_t {
    Normal,
    Veteran,
    Hero,
    Nightmare,
};

// Q16.16 fixed-point scalar (1.0 == 1 << 16).
using Q16_16 = s32;

constexpr Q16_16 ONE = 1 << 16;
constexpr Q16_16 ZERO = 0;

struct ProfileScalars {
    Q16_16 playerDamageTaken = ONE;
    Q16_16 enemyHp = ONE;
    Q16_16 stagger = ONE;
    Q16_16 cooldown = ONE;
    Q16_16 enemyCount = ONE;
    Q16_16 hearts = ONE;
    Q16_16 ammo = ONE;
    Q16_16 rupees = ONE;
    Q16_16 fairyHeal = ONE;
    Q16_16 magicArmorCost = ONE;
};

// Party compensation (independent of difficulty profile). From initial_defaults.
struct PartyScalars {
    Q16_16 enemyCount = ONE;
    Q16_16 enemyHp = ONE;       // per-enemy after count blend (clamped)
    Q16_16 stagger = ONE;
    Q16_16 cooldown = ONE;
    Q16_16 hearts = ONE;
    Q16_16 ammo = ONE;
    Q16_16 rupees = ONE;
    Q16_16 targetDurability = ONE;  // total encounter HP target vs vanilla
};

void init();
void reset();

void setProfile(Profile profile);
Profile currentProfile();
const ProfileScalars& scalars();

Q16_16 mul(Q16_16 a, Q16_16 b);
Q16_16 fromFloat(f32 v);
f32 toFloat(Q16_16 v);

// Round half-up for positive products; used by damage/count integerization.
s32 mulRoundToInt(Q16_16 scalar, s32 base);
// Floor after scale (healing / drop credits integerization).
s32 mulFloorToInt(Q16_16 scalar, s32 base);

Q16_16 clampQ(Q16_16 v, Q16_16 lo, Q16_16 hi);

// Party tables (snapshotted party size; disconnect/down must not shrink later).
PartyScalars partyScalarsFor(u8 partySize);
Q16_16 partyCountMul(u8 partySize);
Q16_16 partyTargetDurability(u8 partySize);
Q16_16 partyStaggerMul(u8 partySize);
Q16_16 partyCooldownSupplyMul(u8 partySize);   // +25%/extra, cap ~2.0
Q16_16 partyAmmoSupplyMul(u8 partySize);      // +45%/extra, cap ~3.0
Q16_16 partyRupeeSupplyMul(u8 partySize);     // +20%/extra, cap ~2.0

// Compound: party × difficulty. countMul already achieved → per-enemy HP blend.
Q16_16 compoundEnemyCount(u8 partySize, const ProfileScalars& profile);
Q16_16 compoundPerEnemyHp(u8 partySize, Q16_16 achievedCountMul, const ProfileScalars& profile);
Q16_16 compoundStagger(u8 partySize, const ProfileScalars& profile);
Q16_16 compoundHearts(u8 partySize, const ProfileScalars& profile);
Q16_16 compoundAmmo(u8 partySize, const ProfileScalars& profile);
Q16_16 compoundRupees(u8 partySize, const ProfileScalars& profile);

u32 desiredEnemyCount(u32 vanillaCount, Q16_16 countMul);
s16 scaleEnemyHealth(s16 baseHealth, Q16_16 hpMul);

// Returns false and writes a short reason if any Q16 identity fails.
bool selfCheck(char* errBuf, size_t errCap);

}  // namespace dusk::coop::difficulty
