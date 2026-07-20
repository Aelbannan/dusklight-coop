# Task 16 — Global Difficulty and Drop Scaling

**Status:** 🟡 PoC in progress (Gate H)

**Depends on:** Task 07 (Inventory and Resources), Task 14 (Enemy Augmentation)

**Gate:** H (Difficulty and Drops)

## Description

Implement a global difficulty system separate from party compensation. Normal, Veteran, Hero, and Nightmare profiles. Implement encounter-level drop budgets with category-specific party and difficulty multipliers.

## Sub-tasks

- [x] Define fixed-point representation (`DifficultyScalar`, Q16.16)
- [x] Implement `DifficultyProfile` struct with all tunables
- [x] Implement Normal, Veteran, Hero, Nightmare presets
- [x] Implement encounter snapshot (party size, profile, counts, seed)
- [x] Implement drop budget credits (healing, arrows, bombs, oil, rupees)
- [x] Implement Strategy A: gate native drop candidates (intercept, classify, consult credit)
- [x] Implement category-specific party multipliers
- [x] Implement difficulty supply scalars (compound with party scalars)
- [ ] Implement need-aware category bias
- [x] Implement deterministic pickup race tie-break
- [x] Implement overflow policy (LeaveForOtherPlayers)

## Design

See [design.md](design.md).
