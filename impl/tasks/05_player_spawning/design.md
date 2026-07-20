# Player Spawning and Actor Ownership — Design

## Proxy actor first

Create a lightweight proxy Link actor before attempting a second full `daAlink_c`. Validates:
- Model-resource reuse
- Camera-independent vs camera-dependent draw behavior
- Independent movement
- Collision
- Shadows and particles
- Split-screen culling
- Input and camera routing

## Do not use a guessed Link parameter bit

Unsafe until every use of Link spawn parameters is audited. Use a PC-only pending-spawn registry:

```cpp
struct PendingPlayerSpawn {
    uint64_t token;
    PlayerId player;
    cXyz position;
    csXyz angle;
};

uint64_t registerPendingPlayerSpawn(PlayerId player, const cXyz& position, const csXyz& angle);
```

The create callback associates the newly created actor ID with the pending token.

## Exact actor-create API

Use the verified overload with `0xFFFF` set ID:

```cpp
fopAcM_create(procName, params, &position, roomNo, &angle, &scale, argument, callback);
```

Do not copy guessed fields from the source actor.

## Secondary Link initialization gates

Skip or redirect on creation:
- Original global player registration
- Save-position initialization
- Global selected-item initialization
- Primary camera initialization
- Event-participant registration
- Horse place and return-place writes
- Turn-restart camera writes
- Midna ownership
- Story-form state mutation
- One-time global resource setup

Create a checklist from every write to: `dComIfGp_setPlayer`, `dComIfGs_*`, `dComIfGp_getCamera`, `dComIfGp_getAttention`, horse/Midna globals, turn-restart state, selected item/equipment fields.

## Actor ownership lifecycle

Every owned actor registration must be removed on:
- Actor deletion
- Room unload
- Stage transition
- Player leave
- Owner death (if actor should cancel)
- Save load

Use process IDs plus generation validation where possible.
