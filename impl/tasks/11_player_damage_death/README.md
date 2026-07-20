# Task 11 — Player Damage, Death, and Revival

**Status:** ⬜ Not Started

**Depends on:** Task 07 (Inventory and Resources), Task 09 (Combat Ownership)

**Gate:** None

## Description

Preserve Link's original damage procedure but route resource, rumble, camera, and game-over dependencies through player-aware context. Implement per-player downed state, fairy auto-revive, teammate revival, and all-players-down game over.

## Sub-tasks

- [ ] Enter `ScopedPlayerContext` before Link damage processing
- [ ] Route health, rupee, oil, magic, bottle, and equipment accessors through per-player resource adapter
- [ ] Route rumble and camera effects through player ownership
- [ ] Intercept zero-health/game-over decision for per-player downed state
- [ ] Implement player-specific damage scaling injection point
- [ ] Implement per-player fairy/bottle auto-revive
- [ ] Implement downed state (clear targets, disable colliders, release grabs, permit teammate revival)
- [ ] Implement teammate revival interaction
- [ ] Implement all-players-down → global game over
- [ ] Implement per-player oxygen/underwater timer
- [ ] Handle environmental failure (void, lava, crushing, drowning) per player
- [ ] Preserve Magic Armor behavior with per-player rupee balance
- [ ] Preserve original damage pipeline (armor checks, special attack handling, reaction selection, vibration)

## Design

See [design.md](design.md).
