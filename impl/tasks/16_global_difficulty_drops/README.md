# Task 16 — Global Difficulty and Drop Scaling

**Status:** ⬜ Not Started

**Depends on:** Task 07 (Inventory and Resources), Task 14 (Enemy Augmentation)

**Gate:** H (Difficulty and Drops)

## Description

Implement a global difficulty system separate from party compensation. Normal, Veteran, Hero, and Nightmare profiles. Implement encounter-level drop budgets with category-specific party and difficulty multipliers.

## Sub-tasks

- [ ] Define fixed-point representation (`DifficultyScalar`, Q16.16)
- [ ] Implement `DifficultyProfile` struct with all tunables
- [ ] Implement Normal, Veteran, Hero, Nightmare presets
- [ ] Implement encounter snapshot (party size, profile, counts, seed)
- [ ] Implement drop budget credits (healing, arrows, bombs, oil, rupees)
- [ ] Implement Strategy A: gate native drop candidates (intercept, classify, consult credit)
- [ ] Implement category-specific party multipliers
- [ ] Implement difficulty supply scalars (compound with party scalars)
- [ ] Implement need-aware category bias
- [ ] Implement deterministic pickup race tie-break
- [ ] Implement overflow policy (LeaveForOtherPlayers)

## Design

See [design.md](design.md).
