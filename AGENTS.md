# AGENTS.md — Dusklight Co-op Mod

## Overview

Dusklight is a reverse-engineered Twilight Princess reimplementation for PC. The co-op mod adds 2-8 player support by creating indexed `daAlink_c` actors, each driven by its own input device and camera.

Co-op code lives in `src/dusk/coop/` / `include/dusk/coop/` and is guarded by `#if TARGET_PC`. The non-PC path is vanilla Wii/GC.

## Project layout

| Path | What |
|------|------|
| `include/dusk/coop/` | Headers for each subsystem |
| `src/dusk/coop/` | Subsystem implementations |
| `src/d/actor/d_a_alink.cpp` | Main Link actor — most coop hooks live here |
| `src/d/actor/d_a_alink_damage.inc` | Included into d_a_alink.cpp, damage path |
| `src/d/actor/d_a_alink_demo.inc` | Included into d_a_alink.cpp, demo/event path |
| `src/d/d_cc_s.cpp`, `d_cc_uty.cpp` | Collision/combat intercepts |
| `src/d/d_camera.cpp` | Camera intercepts |
| `src/d/d_item.cpp` | Item usage intercepts |
| `src/f_op/f_op_actor_mng.cpp` | Actor spawning / culling intercepts |

Build: CMake + Ninja (`build/macos-default-relwithdebinfo/`).

## Architecture

### Runtime singleton

Central state in `dusk::coop::Runtime` (`g_runtime`, accessed via `runtime()`). Holds arrays indexed by `PlayerId` (0-7):

- `players[]` — slot state (joined, enabled, device, view)
- `playerRuntime[]` — loadout, resources, combat state, life state
- `forms[]` — wolf/human form per player
- `horses[]` — horse ownership
- `cameras[]` — camera route per view

### Subsystems

Each is `dusk::coop::<area>::` with `init()`, `reset()`, `tick()` lifecycle:

| Subsystem | Responsibility |
|-----------|---------------|
| `input` | Per-player input snapshots, device assignment, Start-to-join |
| `player` | Spawning/managing indexed `daAlink_c` actors |
| `camera` | Per-view cameras, secondary camera lifecycle |
| `render` | Multi-viewport rendering, split-screen, dual-camera composite |
| `combat` | Hit registration, friendly fire, cut type attribution, fairies, game-over |
| `forms` | Wolf/human transformation per player |
| `horses` | Per-player horse ownership and mounting |
| `enemy` | Enemy source tracking, room-clear blocking |
| `drops` | Enemy drop gates and item distribution |
| `bottles` | Per-player bottle unlock |
| `context` | RAII `ScopedContext` for current player/view/enemy-target |
| `save` | Per-player save data |
| `debug` | Telemetry logging |
| `attention` | Multi-player attention/awareness |
| `inventory` | Per-player inventory state |
| `difficulty` | Difficulty scaling |
| `alink` | Per-player Link actor helpers (owner resolution) |

### Per-viewport notifications

The Aurora/RmlUi overlay supports notifications on every active co-op viewport:

```cpp
dusk::ui::push_toast_to_all_views({
    .type = "warning",
    .title = "Gather Up",
    .content = "Gather up to go to the next room.",
    .duration = std::chrono::seconds(4),
});
```

This renders one copy in each split-screen cell and falls back to a normal full-screen toast in single-view mode. Include `dusk/ui/ui.hpp` when calling it.

### ScopedContext

RAII class that sets `currentPlayer()`, `currentView()`, `currentEnemyTarget()` for the duration of a scope. Stack is asserted empty at frame boundaries — forgetting one or nesting incorrectly will crash in debug.

```cpp
void doSomething() {
    ScopedContext ctx({playerId, viewId, enemyTarget});
    // currentPlayer() == playerId, currentView() == viewId
}
```

### TARGET_PC pattern

Coop code is injected into vanilla game functions via `#if TARGET_PC` blocks. Common patterns:

- **Guard**: early-return or skip native logic when co-op should handle it
- **Override**: replace a value or call with coop-aware equivalent
- **Hook**: call into a coop subsystem at a strategic point (damage, item use, camera update)

```cpp
#if TARGET_PC
    if (dusk::coop::combat::shouldSuppressGameOver(dusk::coop::currentPlayer())) {
        dusk::coop::combat::markPlayerDowned(dusk::coop::currentPlayer());
        return FALSE;
    }
#endif
```

### Bridge functions

`extern "C"` functions in bridge headers (`coop_combat_bridge.h`, `coop_forms_bridge.h`, etc.) let game code call into coop namespace without C++ name mangling issues.

### Per-player Link identification

`dusk::coop::alink::ownerOf(daAlink_c*)` → `PlayerId` — tells you which player a Link actor belongs to. `resolveOwner()` does the same at create-time.

## Common pitfalls

- **ScopedContext required** — `currentPlayer()` / `currentView()` crash if no context is pushed. Always create one before calling functions that read them.
- **Secondary camera may not exist** — `cameraRoute(1)` can return null until Camera 2 finishes `init_phase2`. Check for null before dereferencing.
- **`isEnabled()` / `isCompiledIn()` no longer exist** — they were removed. Game code with `#if TARGET_PC` always runs on PC.
- **`.inc` files** — they're `#include`d into their parent `.cpp`. Don't add separate include guards, and don't treat them as standalone translation units.
- **Arrays are always MAX_LOCAL_PLAYERS (8)** — iterate or index with bounds checking. Unused slots have `joined = false`.
