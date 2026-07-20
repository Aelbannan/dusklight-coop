# Gate E — Resource Adapter

**Status:** ❌ Not started

**Depends on:** Nothing

## Deliverables

- [ ] Player 0 reads/writes original save fields
- [ ] Player 1 uses sidecar resource storage
- [ ] Global bow with separate arrow counts per player
- [ ] Global bottle slot unlock with separate contents per player
- [ ] Original save loads without companion file
- [ ] Mismatched or missing companion recovers safely

## Acceptance criteria

- Player 0 resource changes are reflected in original save
- Player 1 cannot consume Player 0's arrows or rupees
- Both players can equip the same global item simultaneously
- Bottle unlock is global; initial contents go to collector only
- No companion file → Player 1 initializes from global progression
- Corrupt companion → recovery log + reinitialize affected players

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| — | — | — |

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
