# Multiple Eponas — Design

## Per-player horse ownership

```cpp
daHorse_c* horseForPlayer(PlayerId player);
daHorse_c* mountedHorseForLink(const daAlink_c* link);
```

Ownership strict by default: player mounts only their own Epona, whistle summons only their own, others cannot steal/dismiss. Optional later setting for shared mounts.

## Horse creation and registration

Current: waits for global Link, rejects if global pointer non-null, initializes from one global restart, registers as one global horse.

PC behavior: Player 0's canonical horse may use original registration for compatibility. Secondary horses skip singleton rejection. Every horse registers in `runtime().horses[owner]`. Secondary horses do not overwrite `mPlayerPtr[1]`. Horse create uses owner-specific restart/summon data. Deletion clears only owner's slot.

## Resolve horse from Link

`d_a_alink_horse.inc` repeatedly calls `dComIfGp_getHorseActor()`. Replace with:

```cpp
daHorse_c* daAlink_c::getOwnedHorse() const {
    return dusk::coop::horseForPlayer(dusk::coop::playerIndexForActor(this));
}
daHorse_c* daAlink_c::getMountedHorse() const {
    if (!checkHorseRide()) return nullptr;
    return static_cast<daHorse_c*>(mRideAcKeep.getActor());
}
```

Once mounted, `mRideAcKeep` is authority. Compatibility `getHorseActor()`: under Link/player context → that player's horse; outside → Player 0's horse.

## Horse owner execution context

```cpp
class ScopedHorseOwnerContext {
    explicit ScopedHorseOwnerContext(const daHorse_c& horse);
    ~ScopedHorseOwnerContext();
};
```

Static search and collision callbacks must not rely on ambient context. Pass typed callback payload:

```cpp
struct HorseSearchContext {
    daHorse_c* horse;
    PlayerId owner;
    float maximumDistance;
};
```

## Required horse-query audit

Generate checked-in report of every call to `dComIfGp_getHorseActor`, `setHorseActor`, `daAlink_getAlinkActorClass`, `getHorseRestart`, `setHorseRestart`, `mRideAcKeep` access, horse actor searches and callbacks. Classify each: OWNED_HORSE, MOUNTED_HORSE, ACTUAL_COLLIDING_ACTOR, EVENT_HORSE, PLAYER0_COMPATIBILITY, UNSAFE_UNRESOLVED. Gate fails while any UNSAFE_UNRESOLVED exists.

## Horse calling

1. Verify global Epona unlock and stage allowance
2. Reject if wolf/downed/in event/already mounted
3. If horse exists → owner-specific call behavior
4. Else find safe spawn near that player
5. Create horse with owner token
6. Route call sound/camera/prompt to that player

## Mounting

Check: human form, horse ownership, horse not already mounted, distance/angle, no conflicting event, sufficient clearance. On mount: `link->mRideAcKeep.setData(horse)`, `horseSlot(owner).mounted = true`, `horse->onRideFlg()`. Horseback bow/sword/damage resolve mounted horse and rider explicitly.

## Multi-horse collision

Horse vs world/enemy: native. Horse vs horse: ordinary physical with reduced push. Mounted horse vs player: soft avoidance. Horse attack colliders do not damage teammates. Two horses cannot occupy same summon point.

## Horse persistence

Player 0: original horse restart fields authoritative where compatible.

Players 1-7:

```cpp
struct SecondaryHorseSave {
    bool summoned, mountedAtSave;
    std::string stage;
    int8_t room;
    cXyz position;
    s16 yaw;
};
```

Transition: mounted horses recreate with riders if horses allowed. Unmounted secondary horses despawn, remain summonable. Horse-forbidden destinations preserve ownership. Story events use Player 0's event horse unless adapter supports all horses.

## Resource budget

Horse actor allocates per-instance solid heap of 0x6E60 bytes in current implementation, plus shared resources. Eight horses require profiling of solid heaps, models, rein simulation, collision, shadows, audio, camera-dependent draw.
