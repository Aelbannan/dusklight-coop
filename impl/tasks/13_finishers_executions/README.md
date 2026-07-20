# Task 13 — Finishers and Executions

**Status:** ⬜ Not Started

**Depends on:** Task 11 (Player Damage and Death)

**Gate:** None

## Description

Implement execution-claim system for Ending Blow, Mortal Draw, and wolf finishers. Only one player may claim an execution; other attacks should not knock the target away. Normal damage during finisher does not break the sequence.

## Sub-tasks

- [ ] Audit relevant move classes (Ending Blow, finishing stab, down cut, Mortal Draw, wolf jump finish, etc.)
- [ ] Implement `AttackResolution` classification (NormalDamage, HeavyDamage, Execution, NativeSpecial, ScriptedKill)
- [ ] Implement `ExecutionClaim` system (one claim, expires after frames, only claimer's camera)
- [ ] Implement Ending Blow with execution claim
- [ ] Implement Mortal Draw policy (preserve native behavior, no global health zero)
- [ ] Implement wolf finisher execution claim
- [ ] Implement player-side forced-restart conversion (down player instead of room restart)
- [ ] Preserve enemy-specific elemental instant kills

## Design

See [design.md](design.md).
