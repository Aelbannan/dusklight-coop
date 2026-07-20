# Task 03 — Multiple Cameras

**Status:** ⬜ Not Started

**Depends on:** Task 02 (Split-Screen Rendering)

**Gate:** B (Secondary Camera Safety)

## Description

Create additional camera processes through the camera manager. Generalize the PC camera registry from 4 to 8 entries with bounds checks. Ensure secondary-camera destruction does not trigger primary-camera global side effects.

## Sub-tasks

- [ ] Generalize PC camera-manager registry from 4 to 8 entries
- [ ] Add bounds checks to `fopCamM_Create`
- [ ] Add destroy/unregister helper for stale process IDs
- [ ] Route every camera ID through the PC sidecar
- [ ] Prevent secondary-camera destructor side effects (Cameras 1-7)
- [ ] Implement route construction before process creation
- [ ] Implement input owner and attention owner for each camera
- [ ] Test noncontiguous active camera sets (players leave and rejoin)
- [ ] Verify single-player camera behavior unchanged when co-op disabled
- [ ] Add interpolation keyed by all eight view IDs

## Design

See [design.md](design.md).
