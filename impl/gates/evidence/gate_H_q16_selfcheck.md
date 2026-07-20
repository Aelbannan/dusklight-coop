# Gate H — Q16.16 self-check expectations

**Date:** 2026-07-20

Runnable via `dusk::coop::gate_h::runSelfChecks()` at co-op `init` (see `src/dusk/coop/coop_gate_h_selfcheck.cpp`).

| Check | Expectation |
|-------|-------------|
| `mul(1,1)` | `1` |
| `mul(1.5, 1.25)` | `≈1.875` |
| `desiredEnemyCount(4, 1.875)` | `8` (example_encounter) |
| Hearts `4 × 1.25 × 0.40` | floor/round → budget `2` |
| Stagger `1.25 × 1.30` | `≈1.625` |
| 1p Normal count/HP | identity `1.0` |
| `makeSnapshot` twice | identical budgets/scalars |

Party tables: 2p count `1.5`, 4p count `2.5`.
