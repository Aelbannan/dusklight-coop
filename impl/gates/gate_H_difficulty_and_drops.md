# Gate H — Difficulty and Drops

**Status:** ❌ Not started

**Depends on:** Gate E (Resource Adapter), Gate G (Enemy Adapter)

## Deliverables

- [ ] Fixed-point unit tests for Q16.16 multipliers
- [ ] Encounter snapshot reproducibility
- [ ] Native candidate gating (Strategy A) or parsed table implementation (Strategy B)
- [ ] No resource growth proportional to cloned enemy count
- [ ] Free-for-all deterministic pickup race

## Acceptance criteria

- Difficulty profile values applied correctly through fixed-point arithmetic
- Same encounter snapshot produces identical behavior on replay
- Drop credits budgeted at encounter level; clones spend from the same budget
- Two players colliding with same pickup resolves deterministically
- Full player leaves pickup for another player
- Difficulty scalars compound correctly with party scalars

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| — | — | — |

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
