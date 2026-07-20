# Task 20 — Independent Forms

**Status:** 🟡 PoC (Gate I)

**Depends on:** Task 05 (Player Spawning)

**Gate:** I (Independent Forms)

## Description

Every player may independently be human or wolf. This is not cosmetic: includes per-player transformation state, wolf collision, senses, scent, digging, howling, combat, Midna rider visuals, and field targeting. Form compatibility wrappers replace no-argument global queries.

## Sub-tasks

- [x] Implement per-player `PlayerFormState` (current, desired, phase)
- [~] Implement per-player `PlayerMidnaRuntime` (rider visibility, field state, targets) — struct + accessors; rider draw still per-Link
- [x] Implement global transform-save write isolation (Player 0 writes save; others update sidecar only)
- [x] Audit every `checkNowWolf`, `getTransformStatus`, `setTransformStatus` call site
- [x] Classify each call site: CURRENT_LINK, SPECIFIC_ACTOR, VIEW_OWNER, EVENT_PARTICIPANT, STORY_AUTHORITY, GLOBAL_UNLOCK, UNSAFE_UNRESOLVED
- [x] Replace unsafe global queries with player/actor-aware wrappers
- [x] Implement form rules per stage (Either, ForceHuman, ForceWolf, NoTransformation) — stub + demo hooks
- [x] Implement per-viewport senses rendering (only in views whose owner has senses active)
- [ ] Implement transformation constraints (not while mounted/grabbed/downed/in event/using incompatible item)
- [x] Verify standalone `daMidna_c` remains canonical story actor; per-Link Midna riders handle gameplay
- [ ] Implement per-player wolf abilities (senses, scent, digging, howl, lock-on, attack, swim, collision) — senses flag only on proxy
- [~] Handle story transformation events (force participant demo stub; full event adapter later)

## Design

See [design.md](design.md).

## Gate evidence

- [gate_I_independent_forms.md](../../gates/gate_I_independent_forms.md)
- [gate_I_wolf_query_classification.md](../../gates/evidence/gate_I_wolf_query_classification.md)
- [gate_I_working_vs_stubbed.md](../../gates/evidence/gate_I_working_vs_stubbed.md)
