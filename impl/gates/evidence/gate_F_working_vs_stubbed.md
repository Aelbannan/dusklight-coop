# Gate F — Working vs stubbed + test notes

**Date:** 2026-07-20

## Working in this PoC

| Item | Notes |
|------|-------|
| Combat ownership registry | `CombatOwner` by actor id; player + optional `sourceActor` |
| Same-frame hit sidecar | `registerHit` / `noteAtTgHit`; `endFrame` strongest reaction + damage sum |
| Cut-type attribution | Override via `dusk_coop_overrideCutType` + `cc_at_check` / `cc_pl_cut_bit_get` |
| Three-state friendly fire | Ignore / ContactNoDamage / Full in `dCcS` |
| Per-player rumble on damage | `onPlayerDamaged` → Gate C `input::rumble` |
| Fairy from per-player bottles | `tryConsumeFairy` + `checkDeadAction` hook |
| All-downed game over | `shouldSuppressGameOver` / `triggerGameOverIfNeeded` |
| Proxy registration | Create/delete/resolve paths call `registerPlayerActor` |

## Stubbed / deferred

| Item | Why |
|------|-----|
| Proxy sword combat | No AT colliders on `daCoopProxy_c` yet |
| Projectile/bomb ownership | Needs spawn-site registration (Gate F follow-up / Task 12) |
| Proxy receiving damage | No TG damage pipeline on proxy |
| Hit-stop / knockback ownership | Native pause timer still global |
| Per-view camera shake | Not diverted from `dVibration` camera channel |

## Manual test plan

See `impl/gates/gate_F_combat_attribution.md` § How to test.
