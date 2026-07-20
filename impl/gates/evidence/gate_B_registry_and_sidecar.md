# Gate B — Camera registry and sidecar routing

**Date:** 2026-07-20

## Registry expansion

`src/f_op/f_op_camera_mng.cpp` / `include/f_op/f_op_camera_mng.h`:

- Under `ENABLE_LOCAL_COOP && TARGET_PC`, `l_fopCamM_id` is sized to `fopCamM_MAX_CAMERAS` (**8**); otherwise remains **4**.
- `fopCamM_Create` validates `0 <= cameraIndex < fopCamM_MAX_CAMERAS` and returns `fpcM_ERROR_PROCESS_ID_e` on failure.
- Added `fopCamM_GetID`, `fopCamM_ClearID`, `fopCamM_Delete` (delete process by sparse index without compacting).
- `fopCamM_Init` seeds all slots to `fpcM_ERROR_PROCESS_ID_e`.

## Process creation (no double execute)

`dusk::coop::camera::ensureCameras` / `rejoinCameraSlot`:

1. Build sidecar route (input owner, tracked player, attention owner, window, camera-info fields).
2. Call `fopCamM_Create(viewIndex, fpcNm_CAMERA_e, params)` with `params->base.parameters = viewIndex` so `get_camera_id()` matches the sparse slot.
3. Do **not** call `fpcM_Execute` on the camera — the normal process manager schedules it.

`camera::tick()` only resolves `camera_class*` from pending process IDs via `fpcM_SearchByID`.

## Sidecar diversion

`include/dusk/coop/coop_camera_bridge.h` + diverted `dComIfGp_*` inlines in `d_com_inf_game.h`:

- Index `!= 0` never touches original `mCameraInfo[1]` / `mWindow[1]` / `mPlayerInfo[1]`.
- Per-view window objects live in `coop_camera.cpp` (`g_windows`).
- Camera-info fields live on `CameraRoute` (`coop_types.h`).

## Sparse remove / rejoin

- `removeCameraSlot(id)` → `fopCamM_Delete(id)` + clear route; higher indices unchanged.
- `rejoinCameraSlot(id, owner)` recreates only that slot’s mapping and process request.
