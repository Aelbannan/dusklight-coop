# Targeting and Attention Distribution — Design

No enemy attack scheduler. Each enemy chooses a target independently and runs its original attack state machine independently.

## Enemy attention state

```cpp
struct EnemyAttentionState {
    fpc_ProcID targetActor = fpcM_ERROR_PROCESS_ID_e;
    uint32_t lastTargetChangeFrame = 0;
    uint32_t nextTargetEvaluationFrame = 0;
    std::array<float, MAX_LOCAL_PLAYERS> threat{};
};
```

## Target score

```cpp
float scoreTarget(fopAc_ac_c* enemy, PlayerId player) {
    // Returns -FLT_MAX if invalid (downed, wrong room, etc.)
    // Base: -distance * 0.01
    // +30 if line of sight, -20 if not
    // +8 if enemy is facing player
    // +threat[player]
    // -penalty per enemy already targeting player
    // +12 if current target
}
```

## Distribution options

| Mode | Penalty per enemy targeting player |
|------|:---------------------------------:|
| None | 0 |
| Light | 7.5 |
| Balanced | 15 |
| Strong | 30 |

Default: Light or Balanced. Soft penalty, not strict equal-target rule.

## Threat

Initially may omit threat; use only distance, visibility, distribution, stickiness. Later version:

```cpp
void onPlayerDamagesEnemy(fopAc_ac_c* enemy, PlayerId player, int damage) {
    enemyAttentionState(enemy).threat[player] += static_cast<float>(damage) * 1.5f;
}
```

Threat decays over time.

## Retarget rules

- Evaluate every 24-42 frames (deterministic jitter)
- Switch only when: minimum stickiness elapsed, candidate score exceeds current by margin, current target downed/missing/unreachable, or major threat event
- Do not retarget during: wind-up, active attack frames, grabs, scripted animations
