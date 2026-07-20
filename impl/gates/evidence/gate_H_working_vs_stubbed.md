# Gate H — Working vs stubbed + test notes

**Date:** 2026-07-20

## Working in this PoC

| Item | Notes |
|------|-------|
| Difficulty profiles | Normal / Veteran / Hero / Nightmare Q16 scalars |
| Q16.16 self-check | `difficulty::selfCheck` + `gate_h::runSelfChecks` (example encounter 4→8) |
| Encounter snapshot | Party/profile/count/HP/supply frozen at first eligible room scan |
| Drop budget | Shared encounter credits; Strategy A gate on enemy ID drops |
| Clone count scaling | `clonesPerEligibleSource` from snapshotted count mul |
| Armos HP scaling | `perEnemyHpMul` applied to originals + clones |
| Pickup race | Lowest accepting `PlayerId`; full capacity leaves pickup |
| 1p Normal passthrough | No drop gating; identity compounds |

## Stubbed / deferred

| Item | Why |
|------|-----|
| Parsed drop tables (Strategy B) | Prefer A until ROM tables are wired |
| Live stagger/cooldown/damage-taken | Scalars only; enemy/combat adapters later |
| Exact vanilla expected rates | Heuristic per-enemy baseline |
| Automated in-game harness | Manual plan in gate doc |

## Manual test plan

See `impl/gates/gate_H_difficulty_and_drops.md` § How to test.
