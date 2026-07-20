# Implementation Roadmap

## Phase 1 — Safe co-op foundation
- Add co-op sidecar runtime, indexed accessors, runtime assertions preventing invalid original-array indexing.
- Preserve exact single-player path.

**Definition of done:** Co-op disabled behaves identically; sidecars exist; no code indexes original one-slot arrays with index one.

## Phase 2 — Split-screen rendering
- Render one world through two viewports (same camera).
- Viewport/scissor handling, per-view aspect.
- HUD once; disable incompatible post-processing.

## Phase 3 — Second camera
- Create Camera 1, validate two-view path.
- Generalize PC camera registry to eight entries.
- Add interpolation keyed by all eight view IDs.
- Add 2×2, 3×2, 4×2 layout generation.

## Phase 4 — PC-native input
- InputSnapshot, dynamic SDL device registry, press-Start-to-join.
- Legacy bridge for ports 0-3; generalized action bindings; rumble by player.

## Phase 5 — Proxy Player 2
- Spawn second model actor with independent movement and basic collision.
- Camera 1 follows proxy; soft separation; tether and teleport.

## Phase 6 — Full second Link
- Spawn secondary `daAlink_c`, route per-player input, preserve primary global player.
- Audit static/global Link state; support walking, rolling, jumping, swimming, basic sword combat.

## Phase 6A — Independent forms
- Isolate global transform-save writes; make wolf checks player/actor-aware.
- Independent transformation; per-view senses; per-Link Wolf Midna rider and field state.

## Phase 6B — Multiple Eponas
- Eight horse slots and owner spawn records; remove PC horse singleton rejection.
- Route Link horseback code to owned/mounted horse; route horse code to owner.
- Two, then eight simultaneous horses; transition and companion-save persistence.

## Phase 7 — Inventory and resources
- Permanent items global; per-player loadouts, arrows, bombs, oil, rupees, health, magic.
- Global bottle unlock count; per-player bottle contents; free-for-all pickups.

## Phase 8 — Combat
- Register player/projectile ownership; normalized hit events; friendly-fire filtering.
- Route enemy hits to either player; independent health/rumble/camera effects.
- Per-player lock-on; enemy target selection and soft distribution.

## Phase 9 — Death and special combat
- Fairy consumption per player; downed state; teammate revival; all-players-down game over.
- Finisher claims; audit Mortal Draw, wolf finishers, grabs, forced room restarts.

## Phase 10 — Enemy augmentation
- Build enemy adapter registry; whitelist one fodder enemy; sanitize parameters.
- Spawn deterministic clones; room budgets/caps; room-clear state integration.

## Phase 11 — Scaling and difficulty
- Party enemy-count scaling; party durability; stagger resistance; cooldown-only speed scaling.
- Normal, Veteran, Hero, Nightmare profiles; encounter snapshots; per-enemy overrides.

## Phase 12 — Drops and economy
- Encounter drop budgets; category-specific party multipliers; difficulty multipliers.
- Need-weighted category choice; free-for-all collection.

## Phase 13 — Events and transitions
- Player-0 authority; one-camera cutscenes; recreate players after transitions.
- Patch doors, warps, boss intros.

## Phase 14 — Persistence and polish
- Versioned companion save; late-join initialization; migration handling.
- Player HUDs; re-enable/adapt post-processing; profile large encounters.
