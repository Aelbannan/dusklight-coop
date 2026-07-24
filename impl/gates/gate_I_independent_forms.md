# Gate I — Independent Forms

**Status:** 🟡 PoC in progress

**Depends on:** Gate D (Proxy Player)

## Deliverables

- [x] Two Links hold different forms concurrently (one human, one wolf) — **sidecar state PoC**; visual wolf on P1 blocked by proxy
- [x] Secondary transformation does not alter original global transform save status
- [x] Global wolf query audit completed (0 UNSAFE_UNRESOLVED)
- [x] Per-view senses hook (view-owner gated); Midna remains story authority
- [~] Distinct forms survive save/load and transition — companion `form` field; P0 still original save
- [x] Forced-form stage adapter demonstrated (stub API + demo hooks)

## Acceptance criteria

- [x] Player 0 transforming writes global save; Player 1 does not
- [x] All `checkNowWolf()` / `getTransformStatus()` call sites classified
- [x] Zero call sites remain UNSAFE_UNRESOLVED
- [x] Wolf senses effect renders only in viewports whose owner has senses active (multi-view override)
- [x] Stage ForceWolf applies to event participant via `StageFormPolicy` (demo stub)
- [x] Stage `NoTransformation` blocks form changes via `allowsForm`
- [~] Transition preserves independent forms where stage permits (companion form + runtime; full Link recreate TBD)

## Implementation notes (2026-07-20)

### Working

| Area | Detail |
|------|--------|
| Form API | `dusk::coop::forms::*` in `coop_forms.h/.cpp` |
| Save isolation | `writeTransformSaveIfAuthority`; `dComIfGs_setTransformStatus` divert |
| Query divert | `checkNowWolf` → context; Midna → `checkNowWolfAuthority` |
| SPECIFIC_ACTOR | Instance `checkWolf()` on local player pointers |
| Senses | `setSenses` / `sensesActiveForView`; draw-pass env override |
| Stage adapter | `FormRule::{Either,ForceHuman,ForceWolf,NoTransformation}` |
| Proxy PoC | X = toggle sidecar form; Y = toggle senses |

### Blockers (proxy architecture)

| Gap | Impact |
|-----|--------|
| No secondary `daAlink_c` | Cannot run `changeWolf` / wolf moves on P1 |
| Proxy reuses P0 model | Wolf visual on P1 not shown |
| Standalone Midna still binds global Link | Correct for story; riders need full Link |

## How to test

1. Build with a normal PC build.
2. Join P1 (Start on pad 2).
3. Transform P0 to wolf — confirm `global_save_tf=1` in `[coop]` logs.
4. Press **X** on P1 — log `sidecar transform -> wolf`; `global_save_tf` unchanged if P0 human again.
5. Transform P0 back to human — P1 sidecar may stay wolf (different forms in state).
6. With multi-view: enable senses on P0 only — senses post should appear in view 0, not view 1.
7. P1 wolf + **Y** — view 1 senses override; view 0 unaffected when P0 senses off.
8. Call `forms::applyForcedFormDemo(0, Wolf)` from a debug hook / future event adapter — only participant forced.
9. Disable co-op — SP transform + senses unchanged.

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| 2026-07-20 | Form API + save isolation + query divert + senses per-view + classification | PoC coded |
| 2026-07-20 | [gate_I_wolf_query_classification.md](evidence/gate_I_wolf_query_classification.md) | 0 UNSAFE |
| 2026-07-20 | [gate_I_working_vs_stubbed.md](evidence/gate_I_working_vs_stubbed.md) | Documented |

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
