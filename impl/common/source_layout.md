# Recommended Source Layout

```
include/dusk/coop/
    coop.h
    coop_accessors.h
    coop_context.h
    coop_render.h
    coop_camera.h
    coop_input.h
    coop_player.h
    coop_inventory.h
    coop_bottles.h
    coop_pickups.h
    coop_combat.h
    coop_attention.h
    coop_enemy_scaling.h
    coop_difficulty.h
    coop_drops.h
    coop_transition.h
    coop_save.h
    coop_debug.h

src/dusk/coop/
    coop.cpp
    coop_accessors.cpp
    coop_context.cpp
    coop_render.cpp
    coop_camera.cpp
    coop_input.cpp
    coop_player.cpp
    coop_inventory.cpp
    coop_bottles.cpp
    coop_pickups.cpp
    coop_combat.cpp
    coop_attention.cpp
    coop_enemy_scaling.cpp
    coop_difficulty.cpp
    coop_drops.cpp
    coop_transition.cpp
    coop_save.cpp
    coop_debug.cpp
```

Avoid scattering hundreds of unrelated global variables across original files. Original game files should call a compact co-op API.
