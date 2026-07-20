# Task 06 — Movement, Collision, and Tethering

**Status:** ⬜ Not Started

**Depends on:** Task 05 (Player Spawning)

**Gate:** None

## Description

Implement player-player collision policy, soft separation, tethering, and teleport for out-of-bounds secondary players. All players share the original collision world.

## Sub-tasks

- [ ] Disable hard body blocking between players
- [ ] Implement soft separation for overlapping players
- [ ] Implement tether system (warning force then teleport)
- [ ] Implement safe teleport (multiple offset candidates, raycast, clearance checks)
- [ ] Verify all players share the same room/collision world
- [ ] Handle edge cases: lava, void, water, closed doors

## Design

See [design.md](design.md).
