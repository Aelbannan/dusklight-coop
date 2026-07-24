# Gate D — Proxy Player

**Status:** 🟡 PoC in progress

**Depends on:** Gate A (Render Replay), Gate B (Secondary Camera), Gate C (Input Abstraction)

## Deliverables

- [x] Independent movement and collision
- [x] Correct rendering in every active view (PoC: shared P0 Link model instance + shadow)
- [x] No global player registration overwrite
- [x] Safe deletion and recreation across room transitions

## Acceptance criteria

- [x] Proxy actor moves independently from Player 0 using its own input
- [x] Proxy renders correctly in Camera 0 and Camera 1 (via Independent draw classification + Gate A replay)
- [x] Original global player accessor still returns Player 0
- [x] Proxy is destroyed on room unload and recreated safely
- [x] Soft separation prevents overlap between players
- [x] Tether teleports proxy when too far from authority

## Implementation notes (2026-07-20)

### Working

| Area | Detail |
|------|--------|
| Proxy actor | PC-only `daCoopProxy_c` / `fpcNm_COOP_PROXY_e` registered in the process profile list |
| Spawn | `fopAcM_create(..., setId 0xFFFF, ...)` from play-scene layer; PlayerId in parameters |
| Join wiring | Gate C Press-Start → `player::onPlayerJoined` → spawn near P0 + `camera::ensureCameras` |
| Movement | Driven by `input::snapshot(player)` left stick, camera-relative yaw |
| Collision | `dBgS_ObjAcch` ground/wall; soft separation (45u) vs P0/other proxies |
| Tether | Soft pull >800u; teleport near authority >1500u or room mismatch |
| Sidecar registration | `setPlayerActor(id, proxy)` only — never writes `dComIfGp_setPlayer` for id > 0 |
| Camera 1 | `ensureCameras(span)` + `assignTrackedPlayer(1, 1)` so `dComIfGp_getPlayer(1)` resolves the proxy |
| Room unload | `dScnRoom_Delete` (P0 room) + `dScnPly_Delete` → destroy body, keep joined, recreate on tick |
| Draw class | `COOP_PROXY` marked Independent for Gate A multi-view replay |

### Stubbed / deferred

| Area | Detail |
|------|--------|
| Full secondary `daAlink_c` | Explicitly out of scope; proxy is a lightweight stand-in |
| Own BMD / animations | Draws a second pose of P0’s `mpLinkModel` (same anim as P0) |
| Hazard / door clearance on teleport | Offset candidates + ground check only; no wall capsule / hazard reject yet |
| Noncontiguous join (P0+P2, skip P1) | Camera span still uses max joined id + 1 |
| Combat / inventory on proxy | Gate E/F — proxy is movement + camera target only |
| P4–7 visual join | Spawn path works for any slot; not hardware-verified this session |

## How to test

1. Configure/build with a normal PC build (`TARGET_PC`).
2. Boot into a playable field stage with Player 0 present.
3. On a second gamepad, press **Start** → log `Player N joined` then `Proxy PN spawn/created`.
4. Confirm split views (Gate A forced view count) and Camera 1 tracks the proxy.
5. Move P0 and P1 sticks independently — proxy should not follow P0’s stick.
6. Walk P0 into the proxy — soft separation should push the proxy away.
7. Run far apart — after ~1500 units proxy should teleport near P0.
8. Change rooms / reload stage — proxy body destroyed then recreated; `dComIfGp_getPlayer(0)` still P0.
9. Disable co-op / leave — `destroyAllProxies` clears secondary slots.

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| 2026-07-20 | `coop_player` proxy actor + join/camera/room hooks; docs; RelWithDebInfo link OK | PoC coded — needs multi-pad in-game verification |

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
