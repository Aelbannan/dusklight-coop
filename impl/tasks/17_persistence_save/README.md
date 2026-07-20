# Task 17 — Persistence and Save Compatibility

**Status:** ⬜ Not Started

**Depends on:** Task 07 (Inventory and Resources), Task 20 (Independent Forms), Task 21 (Multiple Eponas)

**Gate:** None

## Description

Implement versioned companion save file that preserves original save compatibility. Player 0 remains in original save; players 1-7 use companion. Reconcile on load.

## Sub-tasks

- [ ] Define companion save format (magic, version, CRC, payload)
- [ ] Define secondary-player persisted state (resources, forms, horse data, settings)
- [ ] Implement per-slot file identity (slot-N-save.coop)
- [ ] Implement two-file save/load (order, atomic replace, crash recovery)
- [ ] Implement load reconciliation (derive globals from original, validate secondary, clamp, init missing)
- [ ] Avoid Player 0 duplication in companion
- [ ] Implement save-position policy (only Player 0 persisted; secondary spawn near authority)
- [ ] Implement migration handling for companion version bumps

## Design

See [design.md](design.md).
