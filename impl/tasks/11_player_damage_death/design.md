# Player Damage, Death, and Revival — Design

## Preserve Link's original damage procedure

Do not replace with simplified health subtraction. Instead:
1. Enter `ScopedPlayerContext` before Link damage processing
2. Route health/rupee/oil/magic/bottle/equipment accessors through per-player resource adapter
3. Route rumble and camera effects through player ownership
4. Intercept final zero-health/game-over decision
5. Preserve original action/reaction procedure

## Damage scaling injection point

Scale at one audited point before applying to current player's resource view. Integer units matching game's health representation. Fixed-point multipliers.

```cpp
int scalePlayerDamageUnits(int rawUnits, DamageScalingPolicy policy, const DifficultySnapshot& difficulty);
```

No arbitrary global "maximum nonlethal damage" cap.

## Player-specific feedback

- Rumble → owner controller
- Camera shake → owner view
- Damage flash → owner viewport
- HUD mutation → owner HUD
- Low-health audio → owner/localized

Music remains global.

## Downed state

At zero health:
1. Try personal fairy/bottle auto-revive
2. If unsuccessful, suppress global game over
3. Enter player-specific Downed
4. Clear enemy target references to that player
5. Disable attack and item colliders
6. Release grabs as configured
7. Permit teammate revival (Hold A nearby)

Trigger original global game over only when every active player is downed.

## Environmental failure

- Void/out-of-world: teleport near living teammate, deduct health
- Lava: down or reposition
- Drowning: per-player oxygen and downing
- Crushing: down affected player
- Story capture: preserve global event

Per-player oxygen and underwater timers required as sidecar resources.
