# Movement, Collision, and Tethering — Design

## One shared collision world

All players and enemies remain in the original shared collision and background systems.

## Player-player collision

Initial: disable hard body blocking, allow attacks to pass through teammates when friendly fire off, soft separation to prevent exact overlap.

```cpp
void applySoftSeparation(daAlink_c* a, daAlink_c* b) {
    cXyz delta = b->current.pos - a->current.pos;
    delta.y = 0.0f;
    float distance = delta.abs();
    if (distance <= 0.001f || distance >= 45.0f) return;
    delta.normalize();
    float push = (45.0f - distance) * 0.25f;
    a->current.pos -= delta * push;
    b->current.pos += delta * push;
}
```

## Tethering

```cpp
void updateTether() {
    daAlink_c* p1 = getLink(0);
    daAlink_c* p2 = getLink(1);
    float distance = p1->current.pos.abs(p2->current.pos);

    if (distance > TELEPORT_DISTANCE || p1->current.roomNo != p2->current.roomNo) {
        teleportPlayerNearAuthority(1);
        return;
    }
    if (distance > TETHER_START_DISTANCE)
        applyTetherForce(p2, p1->current.pos);
}
```

## Safe teleport

Try multiple offsets around authority player:

```cpp
static const cXyz offsets[] = {
    { 100.0f, 0.0f, 0.0f },   { -100.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 100.0f },   { 0.0f, 0.0f, -100.0f },
};
```

For each: raycast downward, confirm ground, confirm wall/capsule clearance, reject hazard surfaces, reject closed-door interiors. Place at first valid candidate.
