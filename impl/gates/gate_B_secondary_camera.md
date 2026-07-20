# Gate B — Secondary Camera Safety

**Status:** 🟡 PoC in progress

**Depends on:** Nothing

## Deliverables

- [x] Cameras 1-7 created and normally scheduled through the process manager (no manual extra execute)
- [x] Cameras 1-7 destruction does not write primary turn-restart camera state
- [x] Correct independent input, player, window, and attention routing (attention *object* still global — see stubs)
- [x] Noncontiguous removal/rejoin is safe
- [x] Single-player camera behavior unchanged

## Acceptance criteria

- No double execution of camera processes
- Secondary destructors restrict global side effects to Camera 0 only
- Every camera has its own input owner, player target, attention owner, window, and interpolation namespace
- Removing Camera 3 does not shift or corrupt Cameras 4-7
- Rejoining Camera 3 restores only its own state

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| 2026-07-20 | [gate_B_registry_and_sidecar.md](evidence/gate_B_registry_and_sidecar.md) | ✅ PoC |
| 2026-07-20 | [gate_B_destructor_guards.md](evidence/gate_B_destructor_guards.md) | ✅ PoC |
| 2026-07-20 | [gate_B_working_vs_stubbed.md](evidence/gate_B_working_vs_stubbed.md) | ✅ Notes |

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
