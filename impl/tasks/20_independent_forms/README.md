# Task 20 — Independent Forms

**Status:** ⬜ Not Started

**Depends on:** Task 05 (Player Spawning)

**Gate:** I (Independent Forms)

## Description

Every player may independently be human or wolf. This is not cosmetic: includes per-player transformation state, wolf collision, senses, scent, digging, howling, combat, Midna rider visuals, and field targeting. Form compatibility wrappers replace no-argument global queries.

## Sub-tasks

- [ ] Implement per-player `PlayerFormState` (current, desired, phase)
- [ ] Implement per-player `PlayerMidnaRuntime` (rider visibility, field state, targets)
- [ ] Implement global transform-save write isolation (Player 0 writes save; others update sidecar only)
- [ ] Audit every `checkNowWolf`, `getTransformStatus`, `setTransformStatus`, `getLinkPlayer` call site
- [ ] Classify each call site: CURRENT_LINK, SPECIFIC_ACTOR, VIEW_OWNER, EVENT_PARTICIPANT, STORY_AUTHORITY, GLOBAL_UNLOCK, UNSAFE_UNRESOLVED
- [ ] Replace unsafe global queries with player/actor-aware wrappers
- [ ] Implement form rules per stage (Either, ForceHuman, ForceWolf, NoTransformation)
- [ ] Implement per-viewport senses rendering (only in views whose owner has senses active)
- [ ] Implement transformation constraints (not while mounted/grabbed/downed/in event/using incompatible item)
- [ ] Verify standalone `daMidna_c` remains canonical story actor; per-Link Midna riders handle gameplay
- [ ] Implement per-player wolf abilities (senses, scent, digging, howl, lock-on, attack, swim, collision)
- [ ] Handle story transformation events (force participant, force all, hide nonparticipants, restore after)

## Design

See [design.md](design.md).
