# Gate I — Working vs stubbed + test notes

**Date:** 2026-07-20

## Working in this PoC

| Item | Notes |
|------|-------|
| Per-player `PlayerFormState` | `runtime().forms[id]`; `forms::state` / `beginTransform` / `onTransformComplete` |
| Save-write isolation | P0 → original `mTransformStatus`; P1+ sidecar only (`writeTransformSaveIfAuthority`) |
| `changeWolf` / `changeLink` hooks | Write via forms API; complete after `FLG1_IS_WOLF` set/cleared |
| `checkNowWolf` divert | Context player under co-op (`dusk_coop_checkNowWolf`) |
| Midna story authority | `checkNowWolfAuthority` + Midna call sites updated |
| SPECIFIC_ACTOR cleanup | `player->checkNowWolf()` → `player->checkWolf()` (25 sites) |
| Per-view senses | `ScopedWorldDrawPass` overrides env senses; `wolfeye_effect_check` gated |
| Stage form adapter stub | `FormRule` + `applyForcedFormDemo` / `restoreFormsAfterForcedDemo` |
| Companion save form | Version 2 payload `form` byte for secondaries |
| Proxy PoC form toggle | P1+ **X** toggles sidecar wolf/human; **Y** toggles senses flag |
| Query classification | `impl/gates/evidence/gate_I_wolf_query_classification.md` — 0 UNSAFE_UNRESOLVED |

## Stubbed / deferred (Gate D proxy limits)

| Item | Why |
|------|-----|
| True concurrent wolf **models** | `daCoopProxy_c` is not `daAlink_c`; no `changeWolf`, wolf collision, dig, howl |
| Secondary Midna rider visuals | Requires full secondary Link Midna models |
| Event-participant force for non-P0 story demos | Adapter hooks exist; story events still assume global Link |
| Automated multi-pad harness | Manual test plan in gate doc |

## Clear PoC path to acceptance #1

1. Join P1 (Gate D Start).
2. Transform P0 to wolf (vanilla metamorphose) → global save becomes wolf.
3. Press **X** on P1 → sidecar form becomes wolf while P0 can return to human.
4. Confirm telemetry: `forms global_save_tf` stays with P0; `P1 form=wolf` independently.
5. **Blocker for full visuals:** graduate proxy → secondary `daAlink_c` (post–Gate D).

## Manual test plan

See `impl/gates/gate_I_independent_forms.md` § How to test.
