# Gate F — Combat Attribution

**Status:** 🟡 PoC implemented (in-engine verification pending)

**Depends on:** Gate D (Proxy Player), Gate E (Resource Adapter)

## Deliverables

- [x] Player 1 and Player 2 hit same enemy in one frame correctly (sidecar registration + strongest reaction)
- [x] Correct attacker/cut type used (override + `cc_at_check` / `cc_pl_cut_bit_get`)
- [x] Friendly-fire policy has no unwanted callbacks/effects (Ignore / ContactNoDamage / Full)
- [x] Correct individual damage, rumble, rupee cost, and fairy consumption (skeleton; P0 Link wired)
- [x] All-players-down triggers game over

## Acceptance criteria

- Enemy reads correct cut type from the actual attacking Link, not global Player 0
- Same-frame hits both register; strongest reaction wins
- Three-state friendly fire works: ignore / contact no damage / full
- Damage → correct player health deducted; rumble → correct controller; fairy → correct player's bottle
- Only when every active player is downed does global game over occur

## Implementation notes (2026-07-20)

### Working

| Area | Detail |
|------|--------|
| Ownership registry | `CombatOwner` map keyed by `fpc_ProcID`; `registerPlayerActor` from proxy create/delete |
| Hit sidecar | `noteAtTgHit` from `dCcS::SetAtTgGObjInf`; same-frame resolve in `combat::endFrame` (strongest reaction + summed damage) |
| Cut type | `pushAttackCutType` during AT→TG; `daPy_py_c::getCutType` → `dusk_coop_overrideCutType`; `cc_pl_cut_bit_get` / `cc_at_check` use resolved cut |
| Friendly fire | `ChkNoHitGAtTg` blocks Ignore; `SetAtTgGObjInf` skips `PlusDmg` for ContactNoDamage; callbacks still run |
| Damage rumble | `setDamagePoint` → `combat::onPlayerDamaged` → `input::rumble(currentPlayer)` |
| Fairy / downed | `tryConsumeFairy` via `bottles::*` + `dItemNo_FAIRY_e`; `checkDeadAction` / `checkDeadHP` / `checkRestartDead` suppress game over while teammates alive |
| Game over | `triggerGameOverIfNeeded` → `onForceGameOver` on P0 only when `allPlayersDowned` |
| Proxy as attacker | `fpcNm_COOP_PROXY_e` treated as Link-like in `at_power_check` |

### Stubbed / deferred

| Area | Detail |
|------|--------|
| Proxy sword AT colliders | Proxy has no sword AT yet — cut-type path ready, no live P1 sword hits |
| Projectile ownership | Arrows/bombs/boomerangs not yet registered with `sourceActor` |
| Full reaction routing | Hit-stop / knockback still native; sidecar only tracks strongest winner |
| Secondary Link damage TG | Only P0 `daAlink_c` takes enemy damage today; `applyPlayerDamage` API ready for proxies |
| Camera shake per view | Rumble routed; viewport shake not diverted |
| Magic-armor rupee cost per player | Still follows Gate E / `currentPlayer` context |
| Automated harness | Manual pad test plan below |

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| 2026-07-20 | `coop_combat` registry/hits/FF/cut-type/damage/fairy/game-over; hooks in `d_cc_s`, `d_cc_uty`, `d_a_alink_damage.inc`, `d_a_alink_demo.inc`, `d_a_player.h` | Code complete; runtime test pending |

## How to test

1. Build with `-DENABLE_LOCAL_COOP=ON` (PC).
2. **SP regression:** co-op disabled — sword damage, fairy revive, and game over unchanged.
3. Join P1 (Press Start). Confirm proxy registers in combat overlay / logs.
4. **Cut type:** With two players, have P0 land a Helm Splitter (`CUT_TYPE_HEAD_JUMP`) while a second hit context is active — enemy hit direction / cut bits should follow the attributed attacker (check via `resolvedCutType` / enemy reaction).
5. **Same-frame:** Both players AT the same enemy in one frame — both hits in `g_frameHits`; `endFrame` keeps strongest reaction and sums damage.
6. **Friendly fire:** Default `FriendlyFireMode::Ignore` — player AT vs player TG ignored. Set `ContactNoDamage` — contact callbacks, no `PlusDmg`. Set `Full` — damage applies.
7. **Damage / rumble:** Damage P0 — life drops; rumble on P0's pad only.
8. **Fairy:** Give P0 a fairy bottle; drain life — fairy consumed from P0 bottles; life restored; no game over if P1 alive.
9. **Game over:** Down P0 with P1 still alive — no game over. Down all joined players — `onForceGameOver`.

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
