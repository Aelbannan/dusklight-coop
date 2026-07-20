# Task 10 — Targeting and Attention Distribution

**Status:** ⬜ Not Started

**Depends on:** Task 05 (Player Spawning), Task 09 (Combat Ownership)

**Gate:** None

## Description

Implement per-player lock-on targeting for Link. Implement enemy attention distribution with soft score penalty to prevent dogpiling. No centralized attack scheduler.

## Sub-tasks

- [ ] Implement per-player lock-on target state
- [ ] Implement `EnemyAttentionState` per enemy (target actor, threat, evaluation timing)
- [ ] Implement target score function (distance, LoS, facing, threat, distribution penalty, stickiness)
- [ ] Implement retarget rules (evaluation interval, switch thresholds, locked-during-attack frames)
- [ ] Implement attention distribution mode options (None / Light / Balanced / Strong)
- [ ] Implement threat accumulation and decay
- [ ] Verify enemy AI independently selects targets

## Design

See [design.md](design.md).
