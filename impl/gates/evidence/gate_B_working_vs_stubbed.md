# Gate B — Working vs stubbed + test notes

**Date:** 2026-07-20 (updated 2026-07-21)

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
| Dual full-frame composite | Two painter passes → EFB capture → L/R blit (tiled scissors black Metal) |
| Proxy third-person follow on cam1 | `dCamera_c::Run` early-out when tracked actor is not `daAlink_c` |
| Cam1 audio isolation | Secondary `camera_draw` skips `setAudioCamera` / map audio (see Known issues) |

## Stubbed / deferred

| Item | Why |
|------|-----|
| Per-player `dAttention_c` instances | Still one global attention; only ownership IDs are routed. Full attention objects are later gates. |
| Camera pad input for players 4–7 | `mPadID` still comes from player1 mapping; Gate C input abstraction must feed look/lock for non-legacy ports. |
| Frame interpolation for views 1–7 | `frame_interpolation::record_camera` still early-outs unless `camera_id == 0`. |
| Secondary camera fully interactive without proxy Links | `init_phase2` waits for `get_player_actor`; needs Gate D player spawn. |
| Multi-listener / per-view audio | `Z2Audience::mAudioCamera[1]`, `mSpotMic[1]`, `mNumPlayers==1` — Task 19. |
| Runtime integration test harness | No automated in-game test in this session — see manual plan below. |

## Known issues (do not regress)

Documented in code at the call sites. Summary:

| Symptom | Root cause | Mitigation |
|---------|------------|------------|
| SIGSEGV in `Z2SpotMic::setMicState` on P2 Start | `camera_draw` called `setAudioCamera(..., camera_id=1)` but audio arrays are size 1 | Skip audio/map updates when `camera_id != 0` (`d_camera.cpp`) |
| Both panes follow Link 1 | Frame interp applied cam0 snapshot to every camera / `begin_presentation_camera` stomped `dComIfGd_getView()` (often cam1) | `interp_view` + presentation interp cam0-only; skip presentation interp while split present |
| P0 left/right stick flip on join | Writing half-pane aspect into sim `camera->view.aspect` snapped chase yaw | Full-frame aspect in `preparation()`; pane aspect only in painter proj |
| Dual ready too early / cam1 unusable | `getCameraProcess(1) != nullptr` before `init_phase2` sets `field_0xb0c` | `secondaryCameraInitialized` requires `field_0xb0c != 0` |
| Cam1 create stalls | `init_phase2` waits for sidecar player; ensureCameras ran before proxy resolved | Defer `ensureCameras` until `getPlayerActor(P1+)` exists; same-camera fallback until then |
| Painter pass 1 stomps cam0 | `resolveCamera(1)` fell back to cam0 and force-follow rewrote P0 lookat/yaw | Never resolve incomplete secondary as cam0; force-follow only when pass camera is the real secondary |

## Manual test plan

1. **Baseline:** Build with `ENABLE_LOCAL_COOP=OFF` (or unset). Confirm single-player camera / soft-reset / peep unchanged.
2. **Compile-in:** Build with `ENABLE_LOCAL_COOP=ON`. Boot game; leave co-op disabled at runtime — still one camera.
3. **Enable co-op** (Press-Start path once input gate wires it): call `camera::ensureCameras(N)` for N=2..8 after stage camera 0 exists.
4. **Scheduler:** Confirm N camera processes appear in the process list / overlay; no custom execute loop.
5. **Sparse leave:** `removeCameraSlot(3)` with cams 4–7 present — IDs 4–7 remain; slot 3 cleared.
6. **Rejoin:** `rejoinCameraSlot(3, owner)` restores only slot 3.
7. **Destruct:** Room unload / `destroySecondaryCameras` — primary turn-restart camera data must match pre-destroy Camera 0 pose (secondaries must not overwrite it).
8. **OOB safety:** Never index original `mCameraInfo[1]` / `mWindow[1]` with >0 (assert / ASan if available).
9. **P2 join (play):** Start on pad 2 → no crash; log `Gate B: cam1 initialized`; right pane tracks proxy; P0 stick axes unchanged.
