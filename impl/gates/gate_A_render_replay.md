# Gate A — Render Replay Boundary

**Status:** ❌ Not started

**Depends on:** Nothing

## Deliverables

- [ ] Trace of `dScnPly_Draw` and draw-list consumption
- [ ] Camera-dependent actor classification (independent / dependent-pure / dependent-mutating)
- [ ] Two offset cameras rendered without simulation divergence
- [ ] List of disabled/incompatible effects
- [ ] Frame-state hash equal with one versus two rendered views

## Acceptance criteria

- Simulation counters advance once regardless of number of rendered views
- Actor state is identical whether one or two views render
- Camera-dependent actors look correct in both views
- Depth, particles, shadows, attention cursors, post-processing stay inside their viewport
- Rendering View 1 cannot alter the next simulation tick
- Single-player render output unchanged when co-op disabled

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| — | — | — |

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
