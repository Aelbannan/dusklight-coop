# Task 19 — HUD, Audio, and Presentation

**Status:** ⬜ Not Started

**Depends on:** Task 03 (Multiple Cameras), Task 07 (Inventory and Resources)

**Gate:** None

## Description

Implement per-player HUD elements within each viewport, global full-screen UI (pause, inventory, dialogue), audio listener policy, and duplicate-sound limiting.

## Sub-tasks

- [ ] Define per-player HUD contents (health, ammo, rupees, bottles, action items, lock-on, prompts, damage flash, form/senses state)
- [ ] Implement per-viewport HUD rendering (local to each view, not global)
- [ ] Implement global full-screen UI (pause, inventory, map, dialogue, item-get, game-over, cutscene bars)
- [ ] Implement pause ownership (first requester wins, global pause, one menu, per-player loadout view)
- [ ] Implement audio listener policy (party centroid, Camera 0 orientation, global music)
- [ ] Implement duplicate-sound limiting (per-actor source position, same-frame limiting for identical loud events)
- [ ] Implement per-player camera inversion, vibration, HUD scale, labels/colors
- [ ] Re-enable or adapt post-processing effects per-view
- [ ] Add dynamic render resolution controls

## Design

See [design.md](design.md).
