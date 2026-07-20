# Gate C — Input Abstraction

**Status:** ❌ Not started

**Depends on:** Nothing

## Deliverables

- [ ] All co-op Link/camera input reads use `PlayerInputSnapshot`
- [ ] Aurora owns device handles (no duplicate SDL_OpenGamepad)
- [ ] Fifth controller can join and drive a test actor
- [ ] Action binding getters reject invalid player IDs
- [ ] Reconnect restores identity

## Acceptance criteria

- Players 1-4 work through legacy bridge unchanged
- Player 5 works without a legacy PAD port
- Controller disconnect keeps slot reserved; reconnect restores identity
- Rumble routes through player-to-instance mapping
- Press-Start-to-join works
- Keyboard/mouse controls at most one player

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| — | — | — |

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
