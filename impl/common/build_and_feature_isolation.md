# Build and Feature Isolation

## Feature gates

Add compile and runtime gates:

```cmake
CMake option: none; local co-op is part of every PC build
```

```cpp
Compile guard: TARGET_PC (only where a desktop input/render API is required)
Runtime:       coop.enabled
```

The disabled path should avoid allocating co-op sidecars where practical and must preserve original behavior.

This work requires core-engine changes and should be developed as a maintained fork/feature branch, not assumed to fit entirely in the current public mod SDK.

## Resource and performance budgets

Expected cost sources:
- Rendering the world 2-8×.
- Extra human/wolf Link model and animation heaps.
- Up to 8 Wolf Midna rider model sets.
- Up to 8 horse heaps, rein simulations, shadows, audio sources.
- Extra attention managers, enemies, projectiles, collision objects, particles, audio voices.
- Per-view post-processing targets.

## Performance targets

| Target | Requirement |
|--------|-------------|
| Desktop 2-player | 60 FPS |
| Eight-view mode | 30 FPS minimum |
| High-end desktop 8-view | 60 FPS stretch (dynamic resolution) |
| Mobile/low-end 8-view | Aggressive render scale; feature available |
| Simulation | Never change tick rate to recover GPU performance |

## Dynamic resolution

Allow per-view render resolution to drop while UI remains at output resolution. All viewports in one frame use the same scale initially.

## Culling and LOD

An actor is render-visible if visible in any active view. Do not simulation-cull an actor merely because Camera 0 cannot see it. Audit: actor culling, particle culling, shadow culling, audio virtualization, enemy activation/dormancy, room streaming.

## Cleanup invariants

At room unload or co-op disable: no secondary camera process remains; no secondary Link actor; no owned projectile registry entries; no attention object; no pending pickup/item grants; no active enemy-target context; no pending reinforcement; no render context on stack. Assert in debug builds.

## Compatibility matrix

Maintain checked-in data files for: enemy profiles (duplication, durability, tempo, special-kill, drop, targeting, known issues, tested stages); items; doors/transitions; bosses; minigames; mounts; cutscenes; post-processing.

## Explicitly deferred

- Independent rooms
- Online/network co-op
- Eight standalone story Midna actors (gameplay riders are per-Link)
- Automatic boss scaling beyond player damage taken
- >8 independent views
- Simultaneous independent pause menus
- Personal/private pickups
- Per-player audio listeners/output devices
