# Gate F — Combat Attribution

**Status:** ❌ Not started

**Depends on:** Gate D (Proxy Player), Gate E (Resource Adapter)

## Deliverables

- [ ] Player 1 and Player 2 hit same enemy in one frame correctly
- [ ] Correct attacker/cut type used
- [ ] Friendly-fire policy has no unwanted callbacks/effects
- [ ] Correct individual damage, rumble, rupee cost, and fairy consumption
- [ ] All-players-down triggers game over

## Acceptance criteria

- Enemy reads correct cut type from the actual attacking Link, not global Player 0
- Same-frame hits both register; strongest reaction wins
- Three-state friendly fire works: ignore / contact no damage / full
- Damage → correct player health deducted; rumble → correct controller; fairy → correct player's bottle
- Only when every active player is downed does global game over occur

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| — | — | — |

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
