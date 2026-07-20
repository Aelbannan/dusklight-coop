# Task 18 — Events and Transitions

**Status:** ⬜ Not Started

**Depends on:** Task 05 (Player Spawning), Task 19 (HUD and Audio)

**Gate:** None

## Description

Implement Player 0 authority for story events, dialogue, doors, and stage transitions. Handle event start/end barriers, cutscene camera switching, and player recreation after transitions.

## Sub-tasks

- [ ] Enforce Player-0 authority for transitions (doors, warps, save prompts, story events)
- [ ] Implement non-authority trigger behavior (local prompt, prevent spam)
- [ ] Implement event start barrier (freeze secondaries, hide/reposition, switch to full-screen camera)
- [ ] Implement event end restoration (resolve grants, unhide secondaries, safe-place, restore layout, rebuild attention)
- [ ] Implement stage transition flow (serialize runtime state → destroy secondaries → restore after Player 0 init)
- [ ] Implement same-room door interaction for non-authority players
- [ ] Handle Midna story interactions (canonical standalone Midna for events)
- [ ] Patch boss intros and item-get cinematics for co-op

## Design

See [design.md](design.md).
