# Task 04 — Controller Input

**Status:** 🟡 PoC in progress

**Depends on:** Nothing

**Gate:** C (Input Abstraction)

## Description

Add support for more than four physical controllers on PC. Extend Aurora with a stable controller instance-ID API rather than opening gamepads again from Dusklight. Abstract per-player input behind a snapshot API.

## Sub-tasks

- [x] Add/upstream Aurora narrow public API for controller instances and normalized state
- [x] Implement `PlayerInputSnapshot` struct
- [ ] Route all co-op-aware Link/camera input through snapshots (consumers: Gate D+)
- [x] Implement press-Start-to-join flow
- [x] Generalize action bindings to `MAX_LOCAL_PLAYERS` with bounds checks (settings arrays still 4-wide)
- [ ] Store device preferences in settings (not save) — in-memory identity for now
- [x] Implement reconnect/disconnect handling
- [x] Route rumble through player-to-instance mapping
- [x] Implement keyboard policy (one KB/mouse source controls one player)
- [x] Mirror Players 0-3 into legacy PAD ports 0-3

## Design

See [design.md](design.md).

## PoC notes

See [gate_C_input_abstraction.md](../../gates/gate_C_input_abstraction.md) for working vs stubbed and multi-controller test steps.
