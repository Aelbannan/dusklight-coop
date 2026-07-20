# Gate D — Proxy Player

**Status:** ❌ Not started

**Depends on:** Gate A (Render Replay), Gate B (Secondary Camera), Gate C (Input Abstraction)

## Deliverables

- [ ] Independent movement and collision
- [ ] Correct rendering in every active view
- [ ] No global player registration overwrite
- [ ] Safe deletion and recreation across room transitions

## Acceptance criteria

- Proxy actor moves independently from Player 0 using its own input
- Proxy renders correctly in Camera 0 and Camera 1
- Original global player accessor still returns Player 0
- Proxy is destroyed on room unload and recreated safely
- Soft separation prevents overlap between players
- Tether teleports proxy when too far from authority

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| — | — | — |

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
