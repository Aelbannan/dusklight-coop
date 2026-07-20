# Gate J — Multiple Eponas

**Status:** ❌ Not started

**Depends on:** Gate D (Proxy Player)

## Deliverables

- [ ] Two then eight horse actors coexist simultaneously
- [ ] Each is bound to one owner player
- [ ] No secondary horse overwrites the original global pointer
- [ ] Every mounted Link resolves `mRideAcKeep`
- [ ] Horse AI/collision resolves owner or actual colliding actor
- [ ] Eight-horse heap/render/collision/audio telemetry captured
- [ ] Secondary horse persistence and transitions pass

## Acceptance criteria

- `dComIfGp_getHorseActor()` under player context returns that player's horse; outside context returns Player 0's horse
- Horse creation for Player 1+ skips the singleton rejection and global registration
- Every horseback code path resolves the horse from `mRideAcKeep` or explicit owner lookup
- `dComIfGp_setHorseActor()` not called for secondary horses
- All `dComIfGp_getHorseActor` / `setHorseActor` / `getHorseRestart` / `setHorseRestart` call sites classified: OWNED_HORSE, MOUNTED_HORSE, ACTUAL_COLLIDING_ACTOR, EVENT_HORSE, PLAYER0_COMPATIBILITY, or UNSAFE_UNRESOLVED
- Zero call sites remain UNSAFE_UNRESOLVED
- Eight horses stay within resource budget at configured fallback settings

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| — | — | — |

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
