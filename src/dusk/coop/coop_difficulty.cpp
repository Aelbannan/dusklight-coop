#include "dusk/coop/coop_difficulty.h"

#include "dusk/coop/coop.h"

#include <cstdio>

namespace dusk::coop::difficulty {
namespace {

Profile g_profile = Profile::Normal;
ProfileScalars g_scalars{};

ProfileScalars makeNormal() {
    ProfileScalars s;
    return s;
}

ProfileScalars makeVeteran() {
    ProfileScalars s;
    s.playerDamageTaken = fromFloat(1.50f);
    s.enemyHp = fromFloat(1.15f);
    s.stagger = fromFloat(1.30f);
    s.cooldown = fromFloat(0.90f);
    s.enemyCount = fromFloat(1.25f);
    s.hearts = fromFloat(0.40f);
    s.ammo = fromFloat(0.75f);
    s.rupees = fromFloat(0.75f);
    s.fairyHeal = fromFloat(0.75f);
    s.magicArmorCost = fromFloat(1.50f);
    return s;
}

ProfileScalars makeHero() {
    ProfileScalars s;
    s.playerDamageTaken = fromFloat(2.00f);
    s.enemyHp = fromFloat(1.25f);
    s.stagger = fromFloat(1.50f);
    s.cooldown = fromFloat(0.85f);
    s.enemyCount = fromFloat(1.40f);
    s.hearts = ZERO;
    s.ammo = fromFloat(0.60f);
    s.rupees = fromFloat(0.65f);
    s.magicArmorCost = fromFloat(2.00f);
    return s;
}

ProfileScalars makeNightmare() {
    ProfileScalars s;
    s.playerDamageTaken = fromFloat(2.50f);
    s.enemyHp = fromFloat(1.35f);
    s.stagger = fromFloat(1.75f);
    s.cooldown = fromFloat(0.80f);
    s.enemyCount = fromFloat(1.50f);
    s.hearts = ZERO;
    s.ammo = fromFloat(0.40f);
    s.rupees = fromFloat(0.50f);
    s.magicArmorCost = fromFloat(3.00f);
    return s;
}

Q16_16 supplyMul(u8 partySize, f32 perExtra, f32 cap) {
    if (partySize <= 1) {
        return ONE;
    }
    const f32 raw = 1.0f + perExtra * static_cast<f32>(partySize - 1);
    return fromFloat(raw > cap ? cap : raw);
}

bool fail(char* errBuf, size_t errCap, const char* msg) {
    if (errBuf != nullptr && errCap > 0) {
        std::snprintf(errBuf, errCap, "%s", msg);
    }
    return false;
}

}  // namespace

void init() {
    g_profile = Profile::Normal;
    g_scalars = makeNormal();
}

void reset() { init(); }

void setProfile(Profile profile) {
    g_profile = profile;
    switch (profile) {
    case Profile::Veteran:
        g_scalars = makeVeteran();
        break;
    case Profile::Hero:
        g_scalars = makeHero();
        break;
    case Profile::Nightmare:
        g_scalars = makeNightmare();
        break;
    case Profile::Normal:
    default:
        g_scalars = makeNormal();
        break;
    }
}

Profile currentProfile() { return g_profile; }
const ProfileScalars& scalars() { return g_scalars; }

Q16_16 mul(Q16_16 a, Q16_16 b) {
    return static_cast<Q16_16>((static_cast<s64>(a) * static_cast<s64>(b)) >> 16);
}

Q16_16 fromFloat(f32 v) { return static_cast<Q16_16>(v * 65536.0f + (v >= 0.0f ? 0.5f : -0.5f)); }

f32 toFloat(Q16_16 v) { return static_cast<f32>(v) / 65536.0f; }

s32 mulRoundToInt(Q16_16 scalar, s32 base) {
    if (base == 0) {
        return 0;
    }
    const s64 prod = static_cast<s64>(scalar) * static_cast<s64>(base);
    // Round half-up for positive; symmetric half-away for negative.
    const s64 rounded = (prod >= 0) ? ((prod + 0x8000) >> 16) : -(((-prod) + 0x8000) >> 16);
    s32 out = static_cast<s32>(rounded);
    if (base != 0 && out == 0) {
        out = (base > 0) ? 1 : -1;  // nonzero raw → minimum 1 after scale
    }
    return out;
}

s32 mulFloorToInt(Q16_16 scalar, s32 base) {
    if (base == 0) {
        return 0;
    }
    const s64 prod = static_cast<s64>(scalar) * static_cast<s64>(base);
    return static_cast<s32>(prod >> 16);
}

Q16_16 clampQ(Q16_16 v, Q16_16 lo, Q16_16 hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

Q16_16 partyCountMul(u8 partySize) {
    // initial_defaults: 2→1.5, 3→2.0, 4+→2.5
    if (partySize <= 1) {
        return ONE;
    }
    if (partySize == 2) {
        return fromFloat(1.50f);
    }
    if (partySize == 3) {
        return fromFloat(2.00f);
    }
    return fromFloat(2.50f);
}

Q16_16 partyTargetDurability(u8 partySize) {
    // 1 + 0.75*(n-1)
    if (partySize <= 1) {
        return ONE;
    }
    return fromFloat(1.0f + 0.75f * static_cast<f32>(partySize - 1));
}

Q16_16 partyStaggerMul(u8 partySize) {
    if (partySize <= 1) {
        return ONE;
    }
    return clampQ(fromFloat(1.0f + 0.25f * static_cast<f32>(partySize - 1)), ONE, fromFloat(1.75f));
}

Q16_16 partyHealSupplyMul(u8 partySize) { return supplyMul(partySize, 0.25f, 2.0f); }
Q16_16 partyAmmoSupplyMul(u8 partySize) { return supplyMul(partySize, 0.45f, 3.0f); }
Q16_16 partyRupeeSupplyMul(u8 partySize) { return supplyMul(partySize, 0.20f, 2.0f); }

PartyScalars partyScalarsFor(u8 partySize) {
    PartyScalars p;
    p.enemyCount = partyCountMul(partySize);
    p.targetDurability = partyTargetDurability(partySize);
    // Default per-enemy assuming count fully achieved: clamp(target/count, 1, 1.35)
    if (p.enemyCount > 0) {
        // Approximate via float for clarity; stored as Q16.
        const f32 hp = toFloat(p.targetDurability) / toFloat(p.enemyCount);
        p.enemyHp = clampQ(fromFloat(hp), ONE, fromFloat(1.35f));
    }
    p.stagger = partyStaggerMul(partySize);
    p.cooldown = ONE;  // negligible when count scaling achieved
    p.hearts = partyHealSupplyMul(partySize);
    p.ammo = partyAmmoSupplyMul(partySize);
    p.rupees = partyRupeeSupplyMul(partySize);
    return p;
}

Q16_16 compoundEnemyCount(u8 partySize, const ProfileScalars& profile) {
    return mul(partyCountMul(partySize), profile.enemyCount);
}

Q16_16 compoundPerEnemyHp(u8 partySize, Q16_16 achievedCountMul, const ProfileScalars& profile) {
    // partyHealthMultiplier = clamp(target / countMul, 1.0, 1.35)
    Q16_16 partyHp = ONE;
    if (achievedCountMul > 0) {
        const f32 ratio = toFloat(partyTargetDurability(partySize)) / toFloat(achievedCountMul);
        partyHp = clampQ(fromFloat(ratio), ONE, fromFloat(1.35f));
    }
    return mul(partyHp, profile.enemyHp);
}

Q16_16 compoundStagger(u8 partySize, const ProfileScalars& profile) {
    return mul(partyStaggerMul(partySize), profile.stagger);
}

Q16_16 compoundHearts(u8 partySize, const ProfileScalars& profile) {
    return mul(partyHealSupplyMul(partySize), profile.hearts);
}

Q16_16 compoundAmmo(u8 partySize, const ProfileScalars& profile) {
    return mul(partyAmmoSupplyMul(partySize), profile.ammo);
}

Q16_16 compoundRupees(u8 partySize, const ProfileScalars& profile) {
    return mul(partyRupeeSupplyMul(partySize), profile.rupees);
}

u32 desiredEnemyCount(u32 vanillaCount, Q16_16 countMul) {
    if (vanillaCount == 0) {
        return 0;
    }
    const s32 scaled = mulRoundToInt(countMul, static_cast<s32>(vanillaCount));
    return scaled > 0 ? static_cast<u32>(scaled) : vanillaCount;
}

s16 scaleEnemyHealth(s16 baseHealth, Q16_16 hpMul) {
    if (baseHealth <= 0) {
        return baseHealth;
    }
    const s32 scaled = mulRoundToInt(hpMul, baseHealth);
    return static_cast<s16>(scaled > 32767 ? 32767 : scaled);
}

bool selfCheck(char* errBuf, size_t errCap) {
    if (mul(ONE, ONE) != ONE) {
        return fail(errBuf, errCap, "1*1 != 1");
    }
    if (mul(ONE, fromFloat(1.5f)) != fromFloat(1.5f)) {
        return fail(errBuf, errCap, "1*1.5 != 1.5");
    }
    // 1.5 × 1.25 = 1.875 (example_encounter count)
    const Q16_16 count = mul(fromFloat(1.50f), fromFloat(1.25f));
    if (count < fromFloat(1.874f) || count > fromFloat(1.876f)) {
        return fail(errBuf, errCap, "1.5*1.25 != ~1.875");
    }
    // 4 * 1.875 → 7.5 → round half-up → 8
    if (desiredEnemyCount(4, count) != 8) {
        return fail(errBuf, errCap, "desiredEnemyCount(4,1.875) != 8");
    }
    // Party×difficulty hearts: 1.25 * 0.40 = 0.50; 4 * 0.50 = 2
    const Q16_16 hearts = mul(fromFloat(1.25f), fromFloat(0.40f));
    if (mulFloorToInt(hearts, 4) != 2) {
        return fail(errBuf, errCap, "hearts floor 4*0.5 != 2");
    }
    // Stagger compound: 1.25 * 1.30 = 1.625
    const Q16_16 stag = mul(fromFloat(1.25f), fromFloat(1.30f));
    if (stag < fromFloat(1.624f) || stag > fromFloat(1.626f)) {
        return fail(errBuf, errCap, "1.25*1.30 != ~1.625");
    }
    // One-player Normal compounds ≈ identity
    const ProfileScalars normal = makeNormal();
    if (compoundEnemyCount(1, normal) != ONE) {
        return fail(errBuf, errCap, "1p Normal count != 1");
    }
    if (compoundPerEnemyHp(1, ONE, normal) != ONE) {
        return fail(errBuf, errCap, "1p Normal hp != 1");
    }
    // Damage rounding: nonzero min 1
    if (mulRoundToInt(fromFloat(0.01f), 1) != 1) {
        return fail(errBuf, errCap, "min-1 rounding failed");
    }
    // Snapshot reproducibility of party tables
    if (partyCountMul(2) != fromFloat(1.50f) || partyCountMul(4) != fromFloat(2.50f)) {
        return fail(errBuf, errCap, "party count table mismatch");
    }
    if (errBuf != nullptr && errCap > 0) {
        errBuf[0] = '\0';
    }
    return true;
}

}  // namespace dusk::coop::difficulty
