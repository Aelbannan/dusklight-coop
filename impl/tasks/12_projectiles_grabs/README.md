# Task 12 — Projectiles, Bombs, Returning Items, and Grabs

**Status:** ⬜ Not Started

**Depends on:** Task 09 (Combat Ownership)

**Gate:** None

## Description

Implement per-player ownership for projectiles, bombs, boomerangs, clawshots, and enemy grabs. Ensure arrows consume from correct player, bombs return to owner, and grabs capture the correct Link.

## Sub-tasks

- [ ] Implement arrow ownership (consume from firing player, route hit credit)
- [ ] Implement bomb ownership (consumed from bag on place, owner-damage allowed, teammate block)
- [ ] Implement boomerang and clawshot ownership (return to owning Link)
- [ ] Implement enemy grab state (captured actor ID, affect only grabbed Link)
- [ ] Allow other players to interrupt enemy that grabbed a teammate
- [ ] Register all projectile/attack actors in combat ownership registry

## Design

See [design.md](design.md).
