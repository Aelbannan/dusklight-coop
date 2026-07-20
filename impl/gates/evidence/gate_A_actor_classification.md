# Gate A Evidence — Actor Draw Classification

Seed table implemented in `dusk::coop::render::classifyActor` (`src/dusk/coop/coop_render.cpp`). Default for unlisted `procName`: **Independent**.

## Classes

| Class | Meaning | Multi-view rule |
|-------|---------|-----------------|
| **Independent** | Draw does not read the active camera/view | Submit in every view (or once if HUD-only) |
| **DependentPure** | Reads camera/view to build matrices / sort keys but must not mutate durable actor state | Safe to raster per view if packets were built once *or* rebuilt from current view without writes |
| **DependentMutating** | Draw/execute helpers write actor fields from camera 0 | Must move mutation into simulation or camera-independent prep before true multi-camera |

## Seed table

| Proc | Class | Rationale |
|------|-------|-----------|
| `CAMERA` / `CAMERA2` | DependentPure | View authority |
| `Obj_Flag` / `Flag2` / `Flag3` | DependentPure | Camera-facing billboards |
| `Obj_Yousei`, `GRASS` | DependentPure | View-dependent orientation / wind look |
| `KYTAG03` / `04` / `10`, `KYEFF` / `KYEFF2` | DependentPure | Sample camera eye for extents/effects |
| `METER2`, `MENUWINDOW`, `MSG_OBJECT`, `GAMEOVER`, `TIMER` | Independent | Global HUD — draw once after world views |
| `ALINK`, `MIDNA`, `HORSE` | Independent | Body draw; attention cursors are separate (later per-view) |
| `E_RDY`, `E_PZ`, `E_MK`, `E_HZELDA`, `B_DS`, `B_GND` | DependentMutating | Known camera-eye copy into actor fields during demo/helpers |

## Gaps

- Full actor audit of `fpcM_Draw` callbacks is incomplete; treat the table as living evidence.
- Attention cursor draw currently runs once in `dScnPly_Draw` (camera 0).
- Particle *calc* is once; particle *draw* uses per-pass `JPADrawInfo` (DependentPure at raster time).
