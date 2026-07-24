# Gate B — Native camera registry and routing

**Date:** 2026-07-20

## Registry expansion

`src/f_op/f_op_camera_mng.cpp` / `include/f_op/f_op_camera_mng.h`:

- `l_fopCamM_id` is sized to `fopCamM_MAX_CAMERAS` (**8**) as an engine capability.
- `fopCamM_Create` validates `0 <= cameraIndex < fopCamM_MAX_CAMERAS` and returns `fpcM_ERROR_PROCESS_ID_e` on failure.
- Added `fopCamM_GetID`, `fopCamM_ClearID`, `fopCamM_Delete` (delete process by sparse index without compacting).
- `fopCamM_Init` seeds all slots to `fpcM_ERROR_PROCESS_ID_e`.

## Process creation (no double execute)

`dusk::coop::camera::ensureCameras` / `rejoinCameraSlot`:

1. Populate the native camera/window/player slots and build the runtime route metadata.
2. Call `fopCamM_Create(viewIndex, fpcNm_CAMERA_e, params)` with `params->base.parameters = viewIndex` so `get_camera_id()` matches the sparse slot.
3. Do **not** call `fpcM_Execute` on the camera — the normal process manager schedules it.

`camera::tick()` only resolves `camera_class*` from pending process IDs via `fpcM_SearchByID`.

## Native engine storage

`dComIfG_play_c` and its normal `dComIfGp_*` accessors now support eight indexed slots:

- `mCameraInfo[8]`
- `mWindow[8]`
- `mPlayerInfo[8]`
- `mPlayerStatus[8][4]`

## Sparse remove / rejoin

- `removeCameraSlot(id)` → `fopCamM_Delete(id)` + clear route; higher indices unchanged.
- `rejoinCameraSlot(id, owner)` recreates only that slot’s mapping and process request.
