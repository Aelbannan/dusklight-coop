# Task 07 — Inventory and Resources

**Status:** ⬜ Not Started

**Depends on:** Task 05 (Player Spawning)

**Gate:** E (Resource Adapter)

## Description

Implement per-player resource management with global permanent items. Player 0 backs onto original save; secondary players use a `PlayerResourceView` adapter that reads/writes sidecar storage. Includes bottle-slot unlock system and per-player bottle contents.

## Sub-tasks

- [ ] Implement `PlayerResourceView` interface
- [ ] Implement `OriginalPlayerResourceView` (Player 0 → original save)
- [ ] Implement `SidecarPlayerResourceView` (Players 1-7 → sidecar)
- [ ] Define per-player mutable resources (health, arrows, bombs, oil, rupees, magic, oxygen)
- [ ] Keep permanent item unlocks global in original save
- [ ] Implement per-player equipment/action-item selection
- [ ] Implement global bottle-slot unlock mask derived from original save
- [ ] Implement per-player bottle storage (contents + quantities)
- [ ] Implement bottle unlock operation (unlock → initial contents → collector)
- [ ] Implement per-player bottle fill/consume searches
- [ ] Implement max-health progression with per-player current health
- [ ] Implement late-join initialization for inventory
- [ ] Implement item grant refactoring (global capability vs per-player resources)
- [ ] Implement equipment availability (global) vs selection (per-player)

## Design

See [design.md](design.md).
