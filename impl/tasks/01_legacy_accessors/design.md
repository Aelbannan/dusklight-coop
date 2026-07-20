# Legacy Accessor Compatibility — Design

Many original systems call global accessors. Replace the wrapper boundary, not the original structure layout.

## Co-op-aware player accessor

```cpp
fopAc_ac_c* coopGetPlayer(uint32_t index) {
    if (!dusk::coop::runtime().enabled || index == 0)
        return g_dComIfG_gameInfo.play.getPlayer(0);

    if (index >= dusk::coop::MAX_LOCAL_PLAYERS) return nullptr;
    return dusk::coop::runtime().players[index].actor;
}
```

## Co-op-aware window accessor

```cpp
dDlst_window_c* coopGetWindow(uint32_t index) {
    if (!dusk::coop::runtime().enabled || index == 0)
        return g_dComIfG_gameInfo.play.getWindow(0);

    if (index >= dusk::coop::MAX_LOCAL_VIEWS) return nullptr;
    return &dusk::coop::runtime().windows[index];
}
```

## Scoped player context

```cpp
namespace dusk::coop {
class ScopedPlayerContext {
public:
    explicit ScopedPlayerContext(PlayerId player)
        : previous_(runtime().activePlayer) {
        runtime().activePlayer = player;
    }
    ~ScopedPlayerContext() { runtime().activePlayer = previous_; }
private:
    PlayerId previous_;
};
}
```

Usage at `daAlink_c::execute()` entry:

```cpp
int daAlink_c::execute() {
#if TARGET_PC
    dusk::coop::ScopedPlayerContext context(
        dusk::coop::playerIndexForActor(this));
#endif
    return executeOriginal();
}
```

## Scoped enemy-target context

```cpp
class ScopedEnemyTargetContext {
public:
    explicit ScopedEnemyTargetContext(daAlink_c* target);
    ~ScopedEnemyTargetContext();
};
```

During enemy execution, the compatibility player accessor may return the enemy's chosen target. Outside, player zero remains the normal authority.
