# Combat Ownership and Collision — Design

## Shared collision world

Use existing AT/TG/CO collision world for all players and enemies.

## Ownership registry

```cpp
struct CombatOwner {
    fpc_ProcID actor;
    Faction faction;
    std::optional<PlayerId> player;
    std::optional<fpc_ProcID> sourceActor;
};
```

Track player ownership for Link actors, arrows, bombs, boomerangs, clawshots, and any spawned attack/effect actor. Remove on deletion or stage unload.

## Native damage plus sidecar events

`dCcS::SetAtTgGObjInf()` sets hit actor IDs and applies native damage accumulation. Do not replace initially. Add a sidecar hit event before native callbacks:

```cpp
struct HitEvent {
    uint64_t frame;
    fpc_ProcID attacker, target;
    std::optional<PlayerId> attackingPlayer, targetPlayer;
    AttackKind kind;
    int rawAttackPower;
    uint32_t nativeAttackType;
    bool shieldContact;
};
```

## Three-state player-player collision policy

```cpp
enum class AttackContactPolicy {
    IgnoreCompletely,
    ContactWithoutDamage,
    Full,
};
```

Examples: teammate sword vs body → Ignore. Teammate boomerang utility → ContactWithoutDamage. Owner bomb vs owner → Full. Owner bomb vs teammate → Ignore.

Decision before: damage accumulation, hit actor assignment, callbacks, hit effects, shield durability, hit-stop, rumble. But utility interactions relying on collision contact may still need callbacks with damage suppressed.

## Correct attacking Link

Enemy handlers that query global Player 0 must resolve from:
1. Direct attacking actor
2. Ownership registry for projectile/child actor
3. Fallback active enemy-target context

Never use target-selected player as substitute for attacking player.

## Same-frame hits

Record all hits against a target in a frame. Native damage accumulates normally. Strongest reaction-priority hit controls reaction. Kill credit goes to final effective hit (deterministic tie-break). Hit-stop takes max, not sum.
