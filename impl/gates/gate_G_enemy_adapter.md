# Gate G — One Enemy Adapter

**Status:** ❌ Not started

**Depends on:** Gate D (Proxy Player), Gate F (Combat Attribution)

## Deliverables

- [ ] Exact parameter map for one fodder enemy
- [ ] Safe deterministic clone placement
- [ ] Nonpersistent set ID (`0xFFFF`)
- [ ] No duplicate switch/event from clone
- [ ] ALLDIE waits for clones and pending waves
- [ ] Bounded heap/collision usage

## Acceptance criteria

- Clone is created with `fopAcM_create` overload using `0xFFFF` set ID
- Clone does not inherit or set any unique progression switch
- Clone placement: valid ground, capsule clearance, no hazard, deterministic
- `daAlldie_c` does not trigger room-clear while clones or pending waves exist
- Maximum per-room clone cap respected
- Clone is not recursively cloned

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| — | — | — |

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
