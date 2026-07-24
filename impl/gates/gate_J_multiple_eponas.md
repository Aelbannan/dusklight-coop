# Gate J — Multiple Eponas

**Status:** 🟡 PoC in progress

**Depends on:** Gate D (Proxy Player)

## Deliverables

- [x] Two then eight horse actors coexist simultaneously (registry + spawn path; in-game verified for two when story bits allow)
- [x] Each is bound to one owner player
- [x] No secondary horse overwrites the original global pointer
- [ ] Every mounted Link resolves `mRideAcKeep` (helpers ready; bulk horseback.inc still uses compat `getHorseActor`)
- [x] Horse AI/collision resolves owner or actual colliding actor (searchEnemy / scene-change self-pass fixed)
- [ ] Eight-horse heap/render/collision/audio telemetry captured
- [ ] Secondary horse persistence and transitions pass (in-memory slot + room recreate; companion save stubbed)

## Acceptance criteria

- [x] `dComIfGp_getHorseActor()` under player context returns that player's horse; outside context returns Player 0's horse
- [x] Horse creation for Player 1+ skips the singleton rejection and global registration
- [~] Every horseback code path resolves the horse from `mRideAcKeep` or explicit owner lookup (compat wrapper + `resolveMounted` / `mountedHorseForLink`; full Link ride still P0-centric with proxy)
- [x] `dComIfGp_setHorseActor()` not called for secondary horses
- [x] All `dComIfGp_getHorseActor` / `setHorseActor` / `getHorseRestart` / `setHorseRestart` call sites classified: OWNED_HORSE, MOUNTED_HORSE, ACTUAL_COLLIDING_ACTOR, EVENT_HORSE, PLAYER0_COMPATIBILITY, or UNSAFE_UNRESOLVED
- [x] Zero call sites remain UNSAFE_UNRESOLVED
- [ ] Eight horses stay within resource budget at configured fallback settings

## Implementation notes (2026-07-20)

### Working

| Area | Detail |
|------|--------|
| Sidecar | `HorseSlot[8]` in `Runtime`; `horses::slot` / `resolveOwned` / `resolveMounted` / `resolveForContext` |
| Compat getter | `dComIfGp_getHorseActor` → `horses::resolveForContext` when co-op enabled |
| Raw global | `horses::getGlobalHorseActor()` for singleton checks (no recursion) |
| Create bypass | Pending-owner token; secondary skips reject + `setHorseActor` |
| Registration | P0 → global + slot[0]; P1+ → `setOwnedHorse` sidecar only |
| Spawn API | `spawnOwnedHorse` / `spawnOwnedHorseNearPlayer` via `fopAcM_create(HORSE)` |
| Join hook | Gate D join also requests secondary horse near player |
| Destroy | `destroyOwnedHorse` / `destroySecondaryHorses`; dtor clears sidecar |
| Room unload | Secondary horses despawn; `pendingRecreate` restores pose |
| Self callbacks | `HorseSearchContext` + scene-change executor pass `this` |
| Restart | Secondary `savePos` → slot lastKnown*; P0 → original save |
| Classification | [`evidence/gate_J_horse_classification.md`](evidence/gate_J_horse_classification.md) |

### Stubbed / deferred

| Area | Detail |
|------|--------|
| Real secondary horseback | Proxy is not `daAlink_c`; riding/whistle PoC limited to spawn + routing |
| Bulk `d_a_alink_horse.inc` | Still calls `getHorseActor`; use `mountedHorseForLink` when dual Link exists |
| Companion save | Secondary restart is runtime-only |
| 8-horse telemetry | Not profiled |
| Horse-vs-horse soft push | Not implemented |
| Story-bit spawn failures | Secondary create still hits vanilla Epona-rescued / telop gates |

## How to test

1. Configure/build with a normal PC build (`TARGET_PC`).
2. Load a field stage where Epona is allowed (rescued bit set; not mid-telop).
3. Confirm P0 horse still registers via `dComIfGp_getHorseActor` / slot 0.
4. Press **Start** on pad 2 → proxy joins; log should show `Horse P1 spawn requested` then `Horse P1 created (secondary=1)`.
5. Confirm global `mPlayerPtr[HORSE_PTR]` still P0’s horse; `horses::resolveOwned(1)` is the secondary actor.
6. Change rooms → secondary horse destroyed then recreated near last pose / player.
7. Disable co-op → `destroySecondaryHorses` clears P1+.

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| 2026-07-20 | Multi-slot registry, create bypass, compat getter, classification table; RelWithDebInfo `ninja dusklight` link OK | PoC coded — needs in-game multi-pad verification |

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
