# Gate A — Render Replay Boundary

**Status:** 🟡 In progress (PoC skeleton)

**Depends on:** Nothing

## Deliverables

- [x] Trace of `dScnPly_Draw` and draw-list consumption
- [x] Camera-dependent actor classification (independent / dependent-pure / dependent-mutating)
- [x] Two offset cameras rendered without simulation divergence *(same-camera tiled views PoC; true offset cameras = Gate B)*
- [x] List of disabled/incompatible effects
- [x] Frame-state hash equal with one versus two rendered views *(hash is sim-only; advances once via `noteSimulationTick`)*

## Acceptance criteria

- Simulation counters advance once regardless of number of rendered views
- Actor state is identical whether one or two views render
- Camera-dependent actors look correct in both views *(partial: same camera replay; DependentMutating still shared)*
- Depth, particles, shadows, attention cursors, post-processing stay inside their viewport *(tiled scissor for world; attention still global; post disabled)*
- Rendering View 1 cannot alter the next simulation tick
- Single-player render output unchanged when co-op disabled

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| 2026-07-20 | [`evidence/gate_A_dscnply_draw_trace.md`](evidence/gate_A_dscnply_draw_trace.md) — draw/prep vs painter split | Done |
| 2026-07-20 | [`evidence/gate_A_actor_classification.md`](evidence/gate_A_actor_classification.md) — seed classify table in `classifyActor` | Done |
| 2026-07-20 | [`evidence/gate_A_incompatible_effects.md`](evidence/gate_A_incompatible_effects.md) — disable hooks in `mDoGph_Painter` | Done |
| 2026-07-20 | Multi-view loop in `mDoGph_Painter` + `ScopedWorldDrawPass` + window sidecars | PoC |
| 2026-07-20 | `lastFrameStateHash` / `simulationTickCounter` (sim path only) | PoC |

## Implementation map

| Piece | Location |
|-------|----------|
| Render API / sidecars / hash / classify | `include/dusk/coop/coop_render.h`, `src/dusk/coop/coop_render.cpp` |
| Sim boundary hooks | `src/d/d_s_play.cpp` (`dScnPly_Draw`) |
| Multi-view draw-list replay | `src/m_Do/m_Do_graphic.cpp` (`mDoGph_Painter`) |
| Frame interp stays camera 0 | `src/dusk/frame_interpolation.cpp` |

## How to test (PoC)

Build with `-DENABLE_LOCAL_COOP=ON`. At runtime:

```cpp
dusk::coop::setEnabled(true);
dusk::coop::render::setForcedViewCount(2);  // same camera, horizontal split
```

Expect: world rasters in two viewports; `simulationTickCounter` matches single-view pacing; `lastFrameStateHash` unchanged when toggling forced view count between 1 and 2 without moving the player. Disable co-op (`setEnabled(false)`) and confirm full-frame single view restored.

## Still stubbed / next

- True secondary camera processes and offset frustums (Gate B)
- Per-view attention draw
- Per-view projection rebuild (aspect tweak applied to particles only)
- Full DependentMutating refactor
- Offscreen compositor path
- Automated hash A/B test harness

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
