# Global Difficulty and Drop Scaling — Design

## Formula

```
Effective value = Base value × Difficulty profile × Party adjustment × Enemy/attack override
```

## Fixed-point representation

```cpp
using DifficultyScalar = int32_t;  // Q16.16
constexpr DifficultyScalar ONE = 1 << 16;
```

Rounding: positive damage → round half up, minimum 1 when raw was nonzero. Healing → floor after scaling unless adapter overrides. Drop credits → fixed-point accumulation, integer item spawn spends exact credit.

## Difficulty profile

```cpp
struct DifficultyProfile {
    DifficultyScalar playerDamageTaken;
    DifficultyScalar ordinaryEnemyDurability;
    DifficultyScalar staggerThreshold;
    DifficultyScalar knockdownResistance;
    DifficultyScalar decisionDelay;
    DifficultyScalar attackCooldown;
    DifficultyScalar ordinaryEnemyCount;
    DifficultyScalar heartSupply, ammoSupply, bombSupply, oilSupply, rupeeSupply;
    DifficultyScalar bottleHealing, fairyHealing, reviveHealth, magicArmorCost;
    bool disableHeartDrops;
    bool disableFairyAutoRevive;
};
```

## Presets

Veteran: damage 1.50×, enemy HP 1.15×, stagger 1.30×, cooldown 0.90×, count 1.25×, hearts 0.40×, ammo 0.75×, rupees 0.75×, magic armor 1.50×.

Hero: damage 2.00×, HP 1.25×, stagger 1.50×, cooldown 0.85×, count 1.40×, hearts disabled, ammo 0.60×, magic armor 2.00×.

Nightmare: damage 2.50×, HP 1.35×, stagger 1.75×, cooldown 0.80×, count 1.50×, hearts disabled, ammo 0.40×, magic armor 3.00×.

## Encounter snapshot

Party size, difficulty profile ID/version, count multiplier, durability multiplier, stagger multiplier, drop supply scalars, deterministic encounter seed. Do not rescale live enemy because player disconnects/is downed/joins mid-fight.

## Drop implementation — Strategy A (preferred)

1. Let native enemy code resolve candidate drop
2. Intercept before actor spawn
3. Classify candidate category/value
4. Consult encounter fixed-point credit
5. Accept, downgrade, replace, or suppress
6. Spend credit when spawned

## Fixed-point credits

```cpp
struct DropCredits {
    int32_t healingQ16, arrowsQ16, bombsQ16, oilQ16, rupeesQ16;
};
```

Credits fractional. Concrete pickup spends exact category value. Augmented enemies start with lower weight. Both original and augmented spend same credits.

## Overflow policy

Default: `LeaveForOtherPlayers`. Full health/arrows/bombs/oil/wallet → pickup remains. Permanent/global pickups always consume once granted.

## Environmental farming

Enemy drops scaled first. Pots/grass/rocks/repeatable room objects remain vanilla initially.
