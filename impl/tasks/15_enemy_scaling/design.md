# Enemy Scaling — Design

## Durability models

```cpp
enum class DurabilityModel {
    NumericHealth,
    HitCount,
    ArmorBreak,
    VulnerabilityPhases,
    Scripted,
};
```

Each enemy adapter describes how scaling applies.

## Party durability target

```cpp
float targetEncounterDurability(uint32_t players) {
    return 1.0f + 0.75f * static_cast<float>(players - 1);
}
```

| Players | Target total durability |
|--------:|:----------------------:|
| 1 | 1.00× |
| 2 | 1.75× |
| 3 | 2.50× |
| 4 | 3.25× |

## Per-enemy health from count

```cpp
float partyHealthMultiplier(uint32_t players, float countMultiplier) {
    return std::clamp(targetEncounterDurability(players) / countMultiplier, 1.0f, 1.35f);
}
```

## Recommended class rules

- **Fodder:** 1.00×-1.10× HP, rely on count
- **Standard:** Up to 1.35× HP from party scaling
- **Elite:** Do not duplicate. Optional 1.15×-1.40× HP
- **Scripted:** Vanilla unless explicitly adapted
- **Boss:** Separate boss-specific handling

## Enemy damage

Player count should not directly increase enemy damage by default. More enemies already increase attack attempts, projectiles, overlap, off-screen threats, movement pressure. Use global difficulty for incoming damage.

## Stagger and knockdown

Co-op focus fire can stun-lock. Add: higher stagger threshold, brief knockdown immunity after recovery, same-frame reaction aggregation, damage still applies during resistance, strong attacks still interrupt.

```cpp
float partyStaggerMultiplier(uint32_t players) {
    return std::min(1.75f, 1.0f + 0.25f * static_cast<float>(players - 1));
}
```

## Attack-speed scaling

Do not globally increase animation playback speed. Keep wind-up, active hitbox, projectile speed, heavy-attack recovery, grab/execution timing at vanilla.

Scale only: decision delay, idle delay, cooldown before choosing another attack.

| Players | Cooldown duration | Frequency |
|--------:|:-----------------:|:---------:|
| 1 | 1.00× | 1.00× |
| 2 | 0.97× | 1.03× |
| 3 | 0.94× | 1.06× |
| 4+ | 0.91× | 1.10× |

Blend toward vanilla when room already reaches desired count multiplier. Do not scale cooldowns for grabs, instant-kill attacks, forced-restart attacks, puzzle enemies, elites, persistent projectile fields, scripted sequences.
