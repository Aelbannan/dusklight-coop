# Gate B — Secondary destructor side-effect guards

**Date:** 2026-07-20

## Primary-only global writes (vanilla)

`dCamera_c::~dCamera_c()` wrote turn-restart camera eye/center/up/fovy and cleared `fopAc_ac_c::setStopStatus(0)` for **every** camera body.

`camera_delete` always called `dComIfGp_setCamera(0, NULL)`, which would clear the primary slot when a secondary was destroyed.

`init_phase2` always called `dComIfGp_getAttention()->Init(player, PAD_1)`, re-binding the single global attention manager.

## Guards added (`local co-op support && TARGET_PC`)

| Site | Behavior |
|------|----------|
| `dCamera_c::~dCamera_c` | If `mCameraID != 0`, skip turn-restart + stop-status writes entirely (member destruction still runs normally). |
| `camera_delete` | Clears `dComIfGp_setCamera(camera_id, NULL)` for the destroying camera’s own ID. |
| `init_phase2` | Calls global `Attention::Init` only when `camera_id == 0`. |

## Verification notes

- Camera 0 path is unchanged when co-op is compiled out or disabled.
- Secondary teardown must go through `fopCamM_Delete` / process delete so `~dCamera_c` runs under the guard.
