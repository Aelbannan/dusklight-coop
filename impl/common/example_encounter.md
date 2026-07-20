# Example End-to-End Encounter

**Setup:**
- Party: 2 players
- Difficulty: Veteran
- Vanilla room: 4 eligible ordinary enemies
- Room type: Large interior

## Count

```
Party multiplier:       1.50×
Veteran base:           1.25×
Raw:                    1.50 × 1.25 = 1.875×
Desired count:          4 × 1.875 = 7.5 → 8 enemies
Spawn:                  4 original + 4 augmented
```

## Durability

```
Veteran global HP:      1.15×
Party target:           1.75× total
Count achieved:         2.00× (from 4 → 8)
Per-enemy HP:           clamp to 1.00×
Effective per-enemy:    1.15×
Total encounter HP:     8 × 1.15 vs 4 × 1.00 vanilla = 2.30× vanilla
```

## Enemy tempo

Veteran cooldown: 0.90× duration. Count already exceeds two-player target, so no extra party cooldown scaling. Attack animations, wind-ups, hitbox durations, projectile speeds remain vanilla.

## Attention

Balanced distribution: 15 points penalty per enemy already targeting a player.

- P1 targeted by 4-5 enemies, P2 by 3-4.
- Distance, visibility, and threat may create temporary dogpiles.

## Damage

Veteran player damage taken: 1.50×. No additional player-count damage multiplier.

## Stagger

```
Veteran stagger:        1.30×
Two-player party:       1.25×
Effective:              1.625× threshold (capped/adjusted per adapter)
```

## Drops

Vanilla expected: 4 heart units, 2 arrow bundles, 1 bomb bundle, 40 rupees.

```
Hearts:   4 × 1.25 (party) × 0.40 (veteran) = 2.00 heart units
Arrows:   2 × 1.45 × 0.75 = 2.175 bundles
Bombs:    1 × 1.45 × 0.75 = 1.0875 bundles
Rupees:  40 × 1.20 × 0.75 = 36 rupees
```

Eight enemies share that total budget. Any player may collect any spawned drop.

## Bottles and resources

Suppose two bottle slots globally unlocked:
- P1: Bottle 0 → Red Potion, Bottle 1 → Empty, Arrows 8, Rupees 102
- P2: Bottle 0 → Fairy, Bottle 1 → Milk, Arrows 25, Rupees 18

If an arrow bundle drops, either player collects. If P2 is full, it remains for P1. Fairy requires empty personal bottle.
