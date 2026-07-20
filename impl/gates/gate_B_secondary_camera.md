# Gate B — Secondary Camera Safety

**Status:** ❌ Not started

**Depends on:** Nothing

## Deliverables

- [ ] Cameras 1-7 created and normally scheduled through the process manager (no manual extra execute)
- [ ] Cameras 1-7 destruction does not write primary turn-restart camera state
- [ ] Correct independent input, player, window, and attention routing
- [ ] Noncontiguous removal/rejoin is safe
- [ ] Single-player camera behavior unchanged

## Acceptance criteria

- No double execution of camera processes
- Secondary destructors restrict global side effects to Camera 0 only
- Every camera has its own input owner, player target, attention owner, window, and interpolation namespace
- Removing Camera 3 does not shift or corrupt Cameras 4-7
- Rejoining Camera 3 restores only its own state

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| — | — | — |

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
