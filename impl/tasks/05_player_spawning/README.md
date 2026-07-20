# Task 05 — Player Spawning and Actor Ownership

**Status:** ⬜ Not Started

**Depends on:** Task 02 (Split-Screen Rendering), Task 04 (Controller Input)

**Gate:** D (Proxy Player)

## Description

Spawn additional Link actors. Start with a lightweight proxy actor to validate model reuse, camera-independent draw, independent movement, and collision. Then graduate to a full secondary `daAlink_c` with initialization gates that skip/redirect global state writes.

## Sub-tasks

- [ ] Implement pending-spawn registry (not actor-parameter bit)
- [ ] Create proxy Link actor with basic movement and rendering
- [ ] Validate camera-independent vs camera-dependent draw behavior on proxy
- [ ] Implement secondary Link initialization gates checklist
- [ ] Audit every global write in Link creation (player, camera, attention, save, horse, Midna globals, turn-restart state)
- [ ] Route per-player input to secondary Link
- [ ] Support walking, rolling, jumping, swimming, basic sword combat
- [ ] Implement actor ownership lifecycle (create, delete, room unload, transition, player leave)
- [ ] Safe destruction of secondary Link across transitions

## Design

See [design.md](design.md).
