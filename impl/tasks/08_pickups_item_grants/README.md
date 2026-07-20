# Task 08 — Pickups and Item Grants

**Status:** ⬜ Not Started

**Depends on:** Task 07 (Inventory and Resources)

**Gate:** None

## Description

Implement free-for-all pickup collection. Resolve same-frame claims deterministically. Handle delayed item grants (chests, shops, NPC gifts, minigames). Define pickup classification (global unlock, consumable, bottle contents, rupee, shared progress).

## Sub-tasks

- [ ] Define `PickupClass` enum
- [ ] Implement `tryCollectPickup()` with classification dispatch
- [ ] Implement deterministic same-frame claim resolution (distance → collision order → player ID)
- [ ] Implement full-resource behavior (leave for other players)
- [ ] Implement `PendingItemGrant` for delayed grants
- [ ] Classify all pickup types according to ownership model
- [ ] Route permanent item grants to global capability
- [ ] Route consumable grants to collecting player's resources

## Design

See [design.md](design.md).
