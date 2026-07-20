# Task 21 — Multiple Eponas

**Status:** 🟡 In Progress (Gate J PoC)

**Depends on:** Task 05 (Player Spawning)

**Gate:** J (Multiple Eponas)

## Description

Every player may own, call, mount, and ride a separate Epona actor. Replace horse singleton with a player-owned registry. Route horse AI and Link horseback code through explicit owner/mounted resolution.

## Sub-tasks

- [x] Implement `HorseSlot[8]` in sidecar runtime
- [x] Implement pending horse-spawn with owner token
- [x] Remove PC horse singleton rejection for secondary horses
- [x] Route `dComIfGp_getHorseActor()` through compatibility wrapper (player context → that player's horse; outside → Player 0)
- [x] Audit all `getHorseActor`, `setHorseActor`, `getHorseRestart`, `setHorseRestart`, `mRideAcKeep` call sites
- [x] Classify each call site: OWNED_HORSE, MOUNTED_HORSE, ACTUAL_COLLIDING_ACTOR, EVENT_HORSE, PLAYER0_COMPATIBILITY, UNSAFE_UNRESOLVED
- [x] Implement `horseForPlayer()`, `mountedHorseForLink()` helpers
- [x] Implement horse execution owner context (`ScopedHorseOwnerContext`)
- [x] Implement horse calling and spawn policy per owner player (spawn API + join hook; whistle still uses compat getter)
- [ ] Implement mount/dismount per-player (resolve through Link's `mRideAcKeep`) — helpers ready; needs secondary `daAlink_c`
- [ ] Implement horseback combat per-player (bow, sword, damage, stirrups, reins)
- [ ] Implement multi-horse collision (soft avoidance between horses)
- [ ] Implement horse persistence (companion save for secondary horses)
- [ ] Implement horse transition rules (create/destroy per destination) — room unload recreate path only
- [ ] Profile eight-horse resource budget (heaps, models, collision, audio)

## Design

See [design.md](design.md).
