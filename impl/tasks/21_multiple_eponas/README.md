# Task 21 — Multiple Eponas

**Status:** ⬜ Not Started

**Depends on:** Task 05 (Player Spawning)

**Gate:** J (Multiple Eponas)

## Description

Every player may own, call, mount, and ride a separate Epona actor. Replace horse singleton with a player-owned registry. Route horse AI and Link horseback code through explicit owner/mounted resolution.

## Sub-tasks

- [ ] Implement `HorseSlot[8]` in sidecar runtime
- [ ] Implement pending horse-spawn with owner token
- [ ] Remove PC horse singleton rejection for secondary horses
- [ ] Route `dComIfGp_getHorseActor()` through compatibility wrapper (player context → that player's horse; outside → Player 0)
- [ ] Audit all `getHorseActor`, `setHorseActor`, `getHorseRestart`, `setHorseRestart`, `mRideAcKeep` call sites
- [ ] Classify each call site: OWNED_HORSE, MOUNTED_HORSE, ACTUAL_COLLIDING_ACTOR, EVENT_HORSE, PLAYER0_COMPATIBILITY, UNSAFE_UNRESOLVED
- [ ] Implement `horseForPlayer()`, `mountedHorseForLink()` helpers
- [ ] Implement horse execution owner context (`ScopedHorseOwnerContext`)
- [ ] Implement horse calling and spawn policy per owner player
- [ ] Implement mount/dismount per-player (resolve through Link's `mRideAcKeep`)
- [ ] Implement horseback combat per-player (bow, sword, damage, stirrups, reins)
- [ ] Implement multi-horse collision (soft avoidance between horses)
- [ ] Implement horse persistence (companion save for secondary horses)
- [ ] Implement horse transition rules (create/destroy per destination)
- [ ] Profile eight-horse resource budget (heaps, models, collision, audio)

## Design

See [design.md](design.md).
