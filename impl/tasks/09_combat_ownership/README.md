# Task 09 — Combat Ownership and Collision

**Status:** ⬜ Not Started

**Depends on:** Task 05 (Player Spawning)

**Gate:** F (Combat Attribution)

## Description

Implement combat ownership registry, normalized hit events, three-state player-player collision policy, and correct attacking-Link resolution. Preserve native damage pipeline; add sidecar hit events for attribution.

## Sub-tasks

- [ ] Implement `CombatOwner` registry (track player/faction for actors and projectiles)
- [ ] Register Link actors, arrows, bombs, boomerangs, clawshots, and spawned attacks
- [ ] Clear ownership on actor deletion and stage unload
- [ ] Add sidecar hit events (`HitEvent`) before native callbacks
- [ ] Implement three-state friendly fire policy (Ignore / ContactNoDamage / Full)
- [ ] Implement correct attacking-Link resolution (from attacker, not global Player 0)
- [ ] Implement same-frame hit aggregation (max reaction, kill credit tie-break)
- [ ] Route rumble, camera shake, and effects to correct player
- [ ] Preserve native damage pipeline (armor, shields, callbacks)

## Design

See [design.md](design.md).
