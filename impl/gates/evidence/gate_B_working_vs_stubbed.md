# Gate B — Working vs stubbed + test notes

**Date:** 2026-07-20

## Working in this PoC

| Item | Notes |
|------|-------|
| 8-slot PC camera process registry | `fopCamM_MAX_CAMERAS == 8` under co-op |
| Bounds-checked create/get/delete | Invalid index → error / no-op |
| `ensureCameras` / `destroy` / `remove` / `rejoin` | Sparse slots; no array compaction |
| Process-manager scheduling only | No manual `fpcM_Execute` on cameras |
| Sidecar camera info / windows / players | Diverted `dComIfGp_*` for index > 0 |
| Per-camera input owner + tracked player + attention owner IDs | Stored on `CameraRoute` |
| Per-camera window | `g_windows[view]` + `assignWindow` |
| Secondary destructor isolation | Turn-restart / stop-status / attention Init / setCamera(0) fixed |
| Single-player unchanged when co-op off | Paths guarded; registry stays size 4 without `ENABLE_LOCAL_COOP` |

## Stubbed / deferred

| Item | Why |
|------|-----|
| Per-player `dAttention_c` instances | Still one global attention; only ownership IDs are routed. Full attention objects are later gates. |
| Camera pad input for players 4–7 | `mPadID` still comes from player1 mapping; Gate C input abstraction must feed look/lock for non-legacy ports. |
| Frame interpolation for views 1–7 | `frame_interpolation::record_camera` still early-outs unless `camera_id == 0`. |
| Secondary camera fully interactive without proxy Links | `init_phase2` waits for `get_player_actor`; needs Gate D player spawn. |
| Runtime integration test harness | No automated in-game test in this session — see manual plan below. |

## Manual test plan

1. **Baseline:** Build with `ENABLE_LOCAL_COOP=OFF` (or unset). Confirm single-player camera / soft-reset / peep unchanged.
2. **Compile-in:** Build with `ENABLE_LOCAL_COOP=ON`. Boot game; leave co-op disabled at runtime — still one camera.
3. **Enable co-op** (Press-Start path once input gate wires it): call `camera::ensureCameras(N)` for N=2..8 after stage camera 0 exists.
4. **Scheduler:** Confirm N camera processes appear in the process list / overlay; no custom execute loop.
5. **Sparse leave:** `removeCameraSlot(3)` with cams 4–7 present — IDs 4–7 remain; slot 3 cleared.
6. **Rejoin:** `rejoinCameraSlot(3, owner)` restores only slot 3.
7. **Destruct:** Room unload / `destroySecondaryCameras` — primary turn-restart camera data must match pre-destroy Camera 0 pose (secondaries must not overwrite it).
8. **OOB safety:** Never index original `mCameraInfo[1]` / `mWindow[1]` with >0 (assert / ASan if available).
