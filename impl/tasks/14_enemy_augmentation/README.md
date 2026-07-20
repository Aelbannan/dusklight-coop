# Task 14 — Enemy Augmentation

**Status:** ⬜ Not Started

**Depends on:** Task 05 (Player Spawning), Task 09 (Combat Ownership)

**Gate:** G (Enemy Adapter)

## Description

Spawn additional ordinary enemies for larger parties using a strict whitelist adapter system. Each adapter documents parameter bits, placement, and behavior. Clones use the exact `fopAcM_create` overload with `0xFFFF` set ID.

## Sub-tasks

- [ ] Define `CoopSpawnClass` and `EnemySpawnAdapter` structures
- [ ] Build enemy adapter registry
- [ ] Whitelist first fodder enemy with complete parameter audit
- [ ] Implement exact create wrapper (0xFFFF set ID, sanitized params)
- [ ] Implement deterministic clone placement (ground, clearance, hazard rejection)
- [ ] Implement non-recursive clone guard (augmented actor ID set)
- [ ] Implement room and performance budgets (max actors, heap, collision)
- [ ] Patch ALLDIE room-clear to account for clones and pending waves
- [ ] Implement progression switch sanitization (clones never set unique switches)
- [ ] Document parameter map for each supported enemy

## Design

See [design.md](design.md).
