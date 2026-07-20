# Task 14 — Enemy Augmentation

**Status:** 🟡 Gate G PoC in progress

**Depends on:** Task 05 (Player Spawning), Task 09 (Combat Ownership)

**Gate:** G (Enemy Adapter)

## Description

Spawn additional ordinary enemies for larger parties using a strict whitelist adapter system. Each adapter documents parameter bits, placement, and behavior. Clones use the exact `fopAcM_create` overload with `0xFFFF` set ID.

## Sub-tasks

- [x] Define `CoopSpawnClass` and `EnemySpawnAdapter` structures *(Gate G: `EnemyAdapter` subset)*
- [x] Build enemy adapter registry
- [x] Whitelist first fodder enemy with complete parameter audit *(Armos / E_AI)*
- [x] Implement exact create wrapper (0xFFFF set ID, sanitized params)
- [x] Implement deterministic clone placement (ground, clearance, hazard rejection)
- [x] Implement non-recursive clone guard (augmented actor ID set)
- [ ] Implement room and performance budgets (max actors, heap, collision) *(per-room clone cap only so far)*
- [x] Patch ALLDIE room-clear to account for clones and pending waves
- [x] Implement progression switch sanitization (clones never set unique switches)
- [x] Document parameter map for each supported enemy *(Armos only)*

## Design

See [design.md](design.md).
