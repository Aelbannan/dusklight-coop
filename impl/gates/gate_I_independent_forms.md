# Gate I — Independent Forms

**Status:** ❌ Not started

**Depends on:** Gate D (Proxy Player)

## Deliverables

- [ ] Two Links hold different forms concurrently (one human, one wolf)
- [ ] Secondary transformation does not alter original global transform save status
- [ ] Global wolf query audit completed
- [ ] Per-view senses and per-player Midna field work
- [ ] Distinct forms survive save/load and transition
- [ ] Forced-form stage adapter demonstrated

## Acceptance criteria

- Player 0 transforming writes global save; Player 1 does not
- All `checkNowWolf()` and `getTransformStatus()` call sites classified: CURRENT_LINK, SPECIFIC_ACTOR, VIEW_OWNER, EVENT_PARTICIPANT, STORY_AUTHORITY, GLOBAL_UNLOCK, or UNSAFE_UNRESOLVED
- Zero call sites remain UNSAFE_UNRESOLVED
- Wolf senses effect renders only in viewports whose owner has senses active
- Stage that forces wolf (e.g., Wolf Link segments) applies only to event participant
- Stage that forbids transformation blocks all players
- Transition preserves independent forms where stage permits

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| — | — | — |

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
