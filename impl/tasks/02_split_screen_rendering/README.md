# Task 02 — Split-Screen Rendering

**Status:** ⬜ Not Started

**Depends on:** Nothing

**Gate:** A (Render Replay Boundary)

## Description

Implement the rendering architecture for multiple simultaneous viewports. This is the highest-priority proof-of-concept. Must establish the replay boundary and per-view rendering before any second player work begins.

## Sub-tasks

- [ ] Instrument `fpcM_Draw` to record camera-dependent behavior (Render Spike)
- [ ] Classify all actor draw work as camera-independent / dependent-pure / dependent-mutating
- [ ] Implement `ScopedRenderView` RAII context (save/restore all global current-render pointers)
- [ ] Implement viewport and scissor handling for sidecar windows
- [ ] Render one world through two viewports (same camera)
- [ ] Fix per-view aspect ratio and FOV
- [ ] Render HUD once only
- [ ] Disable incompatible post-processing effects
- [ ] Add 2×2, 3×2, 4×2 layout generation
- [ ] Implement per-view render resolution scaling
- [ ] Verify simulation state hash is identical with 1 vs 2 rendered views
- [ ] Add direct tiled rendering support
- [ ] Add offscreen rendering compositor interface
- [ ] Implement visibility mask (ViewMask per actor)

## Design

See [design.md](design.md).
