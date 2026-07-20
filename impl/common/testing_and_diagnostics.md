# Testing Plan and Diagnostics

## Single-player regression

Every major co-op patch must verify: co-op disabled, original save loads, original controller behavior, rendering, inventory, enemy behavior, and transitions.

## Rendering tests
- Two viewports render the same camera; two cameras render independently.
- HUD renders once globally; local HUDs stay within their viewports.
- Depth isolated; bloom/fog do not leak; cutscene switches to full-screen and back.
- Frame interpolation uses correct camera history.

## Input tests
- Players 1-4 use independent controllers; Player 5 works without legacy PAD port.
- Controller disconnect/reconnect; reassignment; simultaneous button presses.
- Rumble reaches correct controller; joining does not reorder existing players.

## Inventory tests
- P1 and P2 equip the same global bow; different arrow counts; different bomb types.
- Rupee pickup modifies only collector; full wallet leaves pickup for another.
- Global item acquisition appears for every player; late joiner receives global availability.

## Bottle tests
- Bottle Slot 2 unlocks globally; initial milk goes only to collector.
- Others receive slot empty; P1 cannot use P2's empty bottle.
- Save/reload preserves separate contents; late joiner gets unlocked slots empty.

## Pickup tests
- Two players collide with same drop in one frame; deterministic winner.
- Full-resource player cannot consume; another can collect.
- Delayed chest grant goes to initiating player; permanent rewards unlock globally.

## Combat tests
- P2 sword damages enemy while P1 idle; both hit same enemy in one frame.
- Correct total damage, strongest reaction, kill attribution, player rumble.
- Friendly sword passes through teammate; owner bomb damages owner but not teammate.
- Enemy projectile hits P2 only; independent invulnerability timers and lock-on targets.

## Attention distribution tests
- None mode allows dogpiling; Light softly spreads; Strong trends toward even.
- Nearby player still wins when much closer; enemy does not retarget mid-swing.
- Downed player removed from candidates; no oscillation every frame.

## Finisher tests
- One player claims Ending Blow; another cannot steal it.
- Mortal Draw preserves native enemy handling; boss not globally executed.
- Wolf finisher resolves correct owner.

## Enemy augmentation tests
- Clone receives no persistent set ID; does not inherit forbidden switches.
- Does not recursively clone; placement valid and deterministic; room cap respected.
- Pending waves prevent early room completion; clone death does not trigger unique progression.

## Scaling tests
- One-player Normal remains vanilla; two-player scaling snapshots once per encounter.
- Downing does not reduce enemy HP; disconnecting does not reduce scaling.
- Health/hit-count/cooldown scaling works per adapter.

## Drop tests
- Additional enemies do not multiply supply uncontrollably.
- Party and difficulty multipliers apply correctly.
- Free-for-all awards collector; need-aware selection does not guarantee survival.

## Diagnostics and telemetry

### Debug overlay

Display: player actor IDs, positions/rooms, action states, assigned controller/views, camera IDs/targets, viewport rectangles, active player/enemy-target context, enemy target counts per player, encounter ID, party size/difficulty snapshot, remaining drop budgets, pending reinforcement count, current event/transition state.

### Assertions

```cpp
COOP_ASSERT(player < MAX_LOCAL_PLAYERS);
COOP_ASSERT(view < MAX_LOCAL_VIEWS);
COOP_ASSERT(originalIndex == 0 && "Original one-slot storage indexed with nonzero value");
```

### Deterministic input playback

Record per-frame input snapshots for every active player for reproducing simultaneous attacks, transition bugs, pickup races, finisher conflicts, camera/render issues.

### Encounter metrics

```cpp
struct EncounterMetrics {
    float durationSeconds;
    uint32_t originalEnemyCount, augmentedEnemyCount;
    uint32_t enemyAttackStarts, successfulEnemyHits, offscreenHits;
    uint32_t maximumSimultaneousAttackers;
    std::array<uint32_t, MAX_LOCAL_PLAYERS> enemiesTargetingPeak;
    std::array<int, MAX_LOCAL_PLAYERS> damageReceived, damageDealt;
    DropBudget generatedDrops, collectedDrops;
};
```
