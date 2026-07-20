# Events and Transitions — Design

## Authority

Player 0 owns: dialogue, stage/room-changing doors, warps, story events, item-get cinematics, boss intros, save prompts, Midna story interactions.

Secondary players may operate explicitly whitelisted local switches and destructible objects.

## Non-authority trigger behavior

When secondary player enters global transition trigger: do not request transition, display local "Player 1 must proceed" prompt where appropriate, prevent repeated trigger spam, allow leaving volume normally.

## Event start barrier

Before global event:
1. Stop accepting new player actions
2. Finish/cancel held-item actions per adapter
3. Unregister/destroy temporary player-owned projectiles where unsafe
4. Release enemy grabs
5. Freeze secondary players
6. Hide or reposition them
7. Switch to one full-screen camera
8. Retain explicit item-grant recipient
9. Run original event once

## Event end

1. Resolve pending global/item grants
2. Recreate or unhide secondary players
3. Safe-place near Player 0
4. Restore view layout
5. Rebuild per-player attention
6. Clear stale targets
7. Resume input

## Stage transition

Before unload: serialize runtime resources, forms, horse-slot state into transition snapshot. Dismount or preserve mounted pairs per destination policy. Destroy all secondary cameras (no primary side effects). Delete secondary Links, horses, owned temp actors. Clear ownership registries. Preserve logical resources/forms/ownership — not arbitrary world positions.

After destination Player 0 fully created: create secondary players through pending-spawn registry, restore independent forms (if permitted), safe-place them, recreate mounted/required owned horses, create secondary cameras, restore resources/loadouts, rebuild attention and per-view HUD.

## Same-room doors

Local nonloading doors may be usable by any player after checking: door actor stores initiating player, animation/collision valid for both Links, no double event command, other players not trapped by door state.
