# Gate D — Working vs stubbed + test notes

**Date:** 2026-07-20

## Working in this PoC

| Item | Notes |
|------|-------|
| `daCoopProxy_c` process actor | `fpcNm_COOP_PROXY_e` + `g_profile_COOP_PROXY` (PC / local co-op support) |
| Spawn / destroy / recreate | `spawnProxy` / `destroyProxy` / `onRoomUnload` pending recreate |
| Press-Start join → proxy | `input::tryJoinFromStartPress` → `player::onPlayerJoined` |
| Snapshot-driven move | Left stick via `input::snapshot(id)`; not raw `mDoCPd` for the proxy |
| Soft separate + tether | Design distances (45 / 800 / 1500); P0 immovable under soft sep |
| Camera 1 tracks proxy | Sidecar `getPlayer(1)` → proxy; `ensureCameras` after actor resolves; simple follow in `dCamera_c::Run` |
| Global player 0 preserved | `setPlayerActor` for id>0 only touches `PlayerSlot` / sidecar |
| Build | RelWithDebInfo `ninja dusklight` links with co-op ON |

## Known coupling with Gate B

| Issue | Note |
|-------|------|
| `ensureCameras` before proxy in sidecar | `init_phase2` blocks on `get_player_actor` — join defers create until `getPlayerActor(P1+)` |
| Proxy ≠ `daAlink_c` | Cam1 must not run Alink chase; see `dCamera_c::Run` co-op early-out |
| Shared audio listener | Cam1 must not call `setAudioCamera` — see Gate B evidence known issues |

## Stubbed / deferred

| Item | Why |
|------|-----|
| Dedicated proxy model / anim | Reuses P0 Link model matrix for visibility |
| Full secondary Link init gates | Next graduation after this PoC |
| Safe teleport clearance suite | Ground+offset only |
| Automated in-game harness | Manual pad test plan in gate doc |

## Manual test plan

See `impl/gates/gate_D_proxy_player.md` § How to test.
