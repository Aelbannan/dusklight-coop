# HUD, Audio, and Presentation — Design

## Per-player HUD

Per-view: current health, ammo/bombs/oil, current rupees, bottle contents, action-slot items, downed state, lock-on cursor, local prompts, damage flash, current form/transformation lock, wolf senses state, owned Epona call/mount status, pickup feedback.

Player 0 may reuse original HUD. Secondary players use a new lightweight HUD until original meter system is safely instanced.

## Global UI

Pause menu, inventory, map, dialogue, item-get presentation, save prompt, game-over, cutscene bars, fade.

## Pause ownership

Any active player may request pause. First request wins → `menuOwner`. Simulation pauses globally. One menu instance. Menu displays global items plus owner-specific mutable resources/loadout. Owner or Player 0 closes it. Other players cannot open concurrent menus.

Inventory switching: shoulder buttons may inspect/edit another joined player's loadout; UI clearly identifies selected player. Global item availability unchanged while switching.

## Audio listener

Initial policy:

- Position: centroid of living active players, clamped toward Player 0 if widely separated
- Orientation: Camera 0 orientation
- Music: global
- World sound: ordinary positional audio relative to shared listener
- Player-local feedback: controller rumble and viewport UI; optional nonspatial local mix added later

Pure Player 0 listener is fallback if centroid causes unstable panning.

## Duplicate sounds

Multiple Link/Epona actors may trigger identical sounds on same frame. Add per-actor source position, duplicate-event limiter for identical loud events, no suppression for meaningful separate impacts, global music/event voice unchanged.

## Accessibility

Expose: horizontal/vertical split for 2 players, per-player camera inversion, per-player vibration, HUD scale, player labels/colors, attention distribution mode, pickup overflow policy.
