# Task 15 — Enemy Scaling

**Status:** ⬜ Not Started

**Depends on:** Task 14 (Enemy Augmentation)

**Gate:** None

## Description

Implement party-based enemy health, stagger, and attack-cooldown scaling. Enemy count is the primary scaler; durability and stagger increase modestly. Attack animations remain vanilla speed; only decision delay and cooldown may scale.

## Sub-tasks

- [ ] Implement `DurabilityModel` enum (NumericHealth, HitCount, ArmorBreak, VulnerabilityPhases, Scripted)
- [ ] Implement party durability target formula
- [ ] Implement per-enemy party health multiplier with clamping
- [ ] Implement party stagger multiplier
- [ ] Implement cooldown/decision-delay scaling (conservative, blended when count scaling achieved)
- [ ] Implement durability model dispatch per enemy adapter
- [ ] Document per-enemy override capabilities

## Design

See [design.md](design.md).
