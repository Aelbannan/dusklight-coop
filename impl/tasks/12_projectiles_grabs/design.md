# Projectiles, Bombs, Grabs — Design

## Arrow ownership

1. Check global bow unlock
2. Consume firing player's arrows
3. Spawn arrow, register ownership
4. Route hit credit, rumble, effects to that player

## Bomb ownership

Bomb count consumed from player's bag on placement, not explosion.

Default damage:
- Owner hit by own bomb: damage allowed
- Teammate hit: damage blocked
- Enemy hit: damage allowed
- World object: vanilla

## Boomerang and clawshot

Returning/tethered items store:
- Owning player
- Owning Link actor
- Current target
- Return target
- Source action slot

Never return to global Player 0 accidentally.

## Enemy grabs

```cpp
struct GrabState {
    fpc_ProcID capturedPlayer = fpcM_ERROR_PROCESS_ID_e;
};
```

Grab affects only the captured Link. Other players remain active and may interrupt the enemy.
