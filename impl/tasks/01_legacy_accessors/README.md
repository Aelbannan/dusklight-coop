# Task 01 — Legacy Accessor Compatibility

**Status:** ⬜ Not Started

**Depends on:** Core Runtime (common)

**Gate:** None

## Description

Replace the wrapper boundary around global accessors (getPlayer, getWindow, getCamera) so that co-op-aware code routes through sidecars while unmodified original code still works.

## Sub-tasks

- [ ] Implement `coopGetPlayer(index)` with sidecar fallback
- [ ] Implement `coopGetWindow(index)` with sidecar fallback
- [ ] Implement `ScopedPlayerContext` RAII helper
- [ ] Implement `ScopedEnemyTargetContext` RAII helper
- [ ] Patch `daAlink_c::execute()` entry point to establish player context
- [ ] Add debug assertions preventing invalid original-array indexing

## Design

See [design.md](design.md).
