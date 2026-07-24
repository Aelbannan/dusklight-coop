# Verified Architectural Constraints

Confirmed by source audit against Dusklight `eed14ac` and Aurora `e3d4f82`. These are not design preferences — they are the current reality that any implementation must work within or deliberately replace.

## 2.1 Eight-slot game-state storage behind indexed APIs

`dComIfG_play_c` exposes indexed getters/setters for players, cameras, camera mappings, and windows. The engine storage has been expanded to eight slots:

```cpp
dDlst_window_c mWindow[8];
dComIfG_camera_info_class mCameraInfo[8];
dComIfG_player_info_class mPlayerInfo[8];
u32 mPlayerStatus[8][4];
```

**Consequences:**
- Player/camera/window IDs 0–7 are native engine indices.
- Per-player gameplay resources can remain runtime state where the original game had no indexed concept.
- Every render pass must save and restore current render pointers.

## 2.2 Four-port legacy PAD ABI

GameCube-compatible controller layer is fixed at four ports. Dusklight stores four gamepad interfaces; Aurora defines `PAD_CHANMAX` and `PAD_MAX_CONTROLLERS` as four.

**Consequences:**
- Keep legacy ABI at four.
- Add a player-oriented input API above it.
- Players 4-7 have no legacy PAD port.

## 2.3 Aurora already owns dynamic physical-controller lifetime

Aurora keeps connected controllers in a dynamic map keyed by SDL instance identity.

**Consequences:**
- Dusklight must not call `SDL_OpenGamepad` for devices Aurora already owns.
- Add a narrow public Aurora API for stable instance IDs and normalized state.
- Store co-op player assignments in a separate variable-length settings structure.

## 2.4 Camera manager must generalize to at least eight process slots

Current camera manager stores four process IDs: `static fpc_ProcID l_fopCamM_id[4]`.

**Consequences:**
- PC branch must replace with a bounded camera registry supporting at least 8 entries.
- Every camera-manager API must validate `0 <= cameraIndex < MAX_LOCAL_VIEWS`.
- Eight camera processes are created and normally scheduled.
- Eight windows/viewports, per-camera attention/input owners, interpolation histories, HUD/effect contexts.

## 2.5 One global event manager and one global attention update

The play scene advances one event manager and one global attention object.

**Consequences:**
- Story/event state remains global.
- Additional player attention objects need an explicit manager that runs each one once.
- Only the attention object associated with the current view should draw into that view.

## 2.6 Save data contains one persisted player

`dSv_save_c` contains exactly one `dSv_player_c`.

**Consequences:**
- Do not resize the original save.
- Player 0 remains backed by original save fields.
- Secondary players use the companion save.
- Global capability unlocks must have one source of truth (normally original save).

## 2.7 Actor creation has a verified nonpersistent overload

```cpp
fpc_ProcID fopAcM_create(s16 procName, u16 setId, u32 parameters,
    const cXyz* position, int roomNo, const csXyz* angle,
    const cXyz* scale, s8 argument, createFunc callback);
```

The shorter overload internally supplies set ID `0xFFFF`.

**Consequences:**
- Augmented enemy examples should call this exact API.
- Do not refer to guessed fields such as `source->actor_parameters`.

## 2.8 Horse ownership is currently singleton-based

`getHorseActor()` returns one global pointer. Horse creation refuses if a horse exists. Link horseback code reads `dComIfGp_getHorseActor()` repeatedly.

**Consequences:**
- PC co-op must add `HorseSlot[8]`.
- Horse actor creation requires an explicit owner player.
- Secondary horses skip the singleton rejection/registration.
- `dComIfGp_getHorseActor()` becomes a compatibility wrapper.

## 2.9 Form and Midna state mix per-instance and global assumptions

Wolf Link's models and Midna rider models are per-instance, but changing form calls `dComIfGs_setTransformStatus()` (one global save value). Standalone Midna registers globally and resolves the global Link.

**Consequences:**
- Current form must be stored per player.
- Secondary form changes skip the original global save write.
- Global wolf queries become actor/player-aware.
- Each Link uses its own embedded Wolf Midna models.
- Standalone Midna remains the story authority actor.

## 2.10 Scope exclusions (first release)

- Local-only, one process.
- One shared room and collision world.
- No rollback or network synchronization.
- No independent room streaming.
- No independent story progression.
- No automatic boss duplication.
