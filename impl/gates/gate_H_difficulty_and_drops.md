# Gate H — Difficulty and Drops

**Status:** 🟡 PoC implemented (in-engine verification pending)

**Depends on:** Gate E (Resource Adapter), Gate G (Enemy Adapter)

## Deliverables

- [x] Fixed-point unit tests for Q16.16 multipliers
- [x] Encounter snapshot reproducibility
- [x] Native candidate gating (Strategy A) or parsed table implementation (Strategy B)
- [x] No resource growth proportional to cloned enemy count
- [x] Free-for-all deterministic pickup race

## Acceptance criteria

- Difficulty profile values applied correctly through fixed-point arithmetic
- Same encounter snapshot produces identical behavior on replay
- Drop credits budgeted at encounter level; clones spend from the same budget
- Two players colliding with same pickup resolves deterministically
- Full player leaves pickup for another player
- Difficulty scalars compound correctly with party scalars

## Implementation notes (2026-07-20)

### Working

| Area | Detail |
|------|--------|
| Profiles | Normal / Veteran / Hero / Nightmare via `difficulty::setProfile` (defaults from `initial_defaults.md`) |
| Q16.16 | `mul`, `fromFloat`, `mulRoundToInt`, `mulFloorToInt`; `gate_h::runSelfChecks` + `difficulty::selfCheck` |
| Party scalars | Count 1.5/2.0/2.5, durability target, stagger +25%/player (cap 1.75), drop supply +25%/+45%/+20% |
| Compound | `compoundEnemyCount/Hp/Stagger/Hearts/Ammo/Rupees` = party × difficulty |
| Encounter snapshot | `drops::makeSnapshot` / `ensureEncounter` freezes party size, profile, count/HP/supply muls, budget; `endEncounter` on room unload |
| Clone count | Gate G `clonesForParty` → `drops::clonesPerEligibleSource` from snapshotted count mul |
| Enemy HP | `applySnapshottedHp` on originals + clones (Armos `health` / `field_0x560`) |
| Strategy A | `fopAcM_createItemFromEnemyID` → `gateEnemyDropCandidate` after table roll; pots/grass untouched |
| Shared budget | One encounter `DropBudget`; originals and clones both spend via the same gate |
| Pickup race | `itemGetCoCallBack`: capacity via Gate E `playerCanAcceptItem`; lowest `PlayerId` wins; full leaves pickup |
| 1p Normal | Drop gating skipped; count/HP muls identity → ≈ vanilla |

### Stubbed / deferred

| Area | Detail |
|------|--------|
| Strategy B | Parsed enemy drop tables not required for PoC |
| Vanilla expected rates | Heuristic per-enemy hearts/ammo/rupees (not ROM table parse) |
| Stagger/cooldown apply | Scalars snapshotted; live enemy tempo adapters not wired |
| Player damage-taken | Profile scalar present; combat apply path deferred |
| Need-aware drop bias | Classification only; no weighted re-roll toward needy players |
| Multi-enemy adapters | HP/count still Armos-first (Gate G whitelist) |

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| 2026-07-20 | Q16 self-check + snapshot reproducibility in `coop_gate_h_selfcheck.cpp`; Strategy A gate in `f_op_actor_mng.cpp`; pickup race in `d_a_obj_item.cpp`; count/HP via `coop_enemy.cpp` | Code complete; runtime test pending |

## How to test

1. Build with a normal PC build (PC). Confirm log: `gate_H: Q16 + snapshot self-checks passed`.
2. **SP regression:** co-op off / 1p Normal — enemy drops and Armos HP unchanged.
3. Set Veteran (`difficulty::setProfile`), join P1, enter Armos room:
   - Log `drops: encounter begin …` with frozen party size and budget.
   - Clone count follows `desiredEnemyCount` (room cap still 4).
   - Armos HP ≈ `1000 × perEnemyHpMul`.
4. Kill originals + clones — heart/ammo/rupee spawns stop once budget hits 0 (not ∝ clone count).
5. Two players touch one heart: lower id collects if both need health; full player leaves it.
6. Down/disconnect mid-fight — snapshotted muls/budget unchanged.

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
