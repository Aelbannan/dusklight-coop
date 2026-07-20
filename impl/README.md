# Dusklight Split-Screen Co-op — Implementation Directory

This directory organizes the audited technical design into manageable pieces.

## Structure

```
impl/
  README.md              ← this file
  common/                ← reference documents shared across all tasks
  gates/                 ← proof-of-concept gate definitions and tracking
  tasks/                 ← implementation task folders, each with:
      NN_name/
        README.md        ← task description, checklist, status, dependencies
        design.md        ← extracted design spec for this task
```

## How to use

- **Design decisions** live in `common/` — start here to understand constraints.
- **Proof-of-concept gates** in `gates/` must be cleared before implementation can proceed past them. Each gate lists deliverables and acceptance criteria.
- **Task folders** in `tasks/` each contain a README with a checklist and a design.md extracted from the audited document.

## Task overview

| # | Task | Depends on | Gate |
|---|------|------------|------|
| 01 | Legacy Accessors | Core Runtime | — |
| 02 | Split-Screen Rendering | — | A |
| 03 | Multiple Cameras | 02 | B |
| 04 | Controller Input | — | C |
| 05 | Player Spawning | 02, 04 | D |
| 06 | Movement & Collision | 05 | — |
| 07 | Inventory & Resources | 05 | E |
| 08 | Pickups & Item Grants | 07 | — |
| 09 | Combat Ownership | 05 | F |
| 10 | Targeting & Attention | 05, 09 | — |
| 11 | Player Damage & Death | 07, 09 | — |
| 12 | Projectiles & Grabs | 09 | — |
| 13 | Finishers & Executions | 11 | — |
| 14 | Enemy Augmentation | 05, 09 | G |
| 15 | Enemy Scaling | 14 | — |
| 16 | Difficulty & Drops | 07, 14 | H |
| 17 | Persistence & Save | 07, 20, 21 | — |
| 18 | Events & Transitions | 05, 19 | — |
| 19 | HUD & Audio | 03, 07 | — |
| 20 | Independent Forms | 05 | I |
| 21 | Multiple Eponas | 05 | J |
