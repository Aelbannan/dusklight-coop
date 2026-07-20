# Task 04 — Controller Input

**Status:** ⬜ Not Started

**Depends on:** Nothing

**Gate:** C (Input Abstraction)

## Description

Add support for more than four physical controllers on PC. Extend Aurora with a stable controller instance-ID API rather than opening gamepads again from Dusklight. Abstract per-player input behind a snapshot API.

## Sub-tasks

- [ ] Add/upstream Aurora narrow public API for controller instances and normalized state
- [ ] Implement `PlayerInputSnapshot` struct
- [ ] Route all co-op-aware Link/camera input through snapshots
- [ ] Implement press-Start-to-join flow
- [ ] Generalize action bindings to `MAX_LOCAL_PLAYERS` with bounds checks
- [ ] Store device preferences in settings (not save)
- [ ] Implement reconnect/disconnect handling
- [ ] Route rumble through player-to-instance mapping
- [ ] Implement keyboard policy (one KB/mouse source controls one player)
- [ ] Mirror Players 0-3 into legacy PAD ports 0-3

## Design

See [design.md](design.md).
