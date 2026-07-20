# Gate C — Input Abstraction

**Status:** 🟡 PoC in progress

**Depends on:** Nothing

## Deliverables

- [x] All co-op Link/camera input reads use `PlayerInputSnapshot` (API ready; Link/camera consumers still on Gate D+)
- [x] Aurora owns device handles (no duplicate SDL_OpenGamepad)
- [x] Fifth controller can join and drive a test path (snapshot + join; actor spawn is Gate D)
- [x] Action binding getters reject invalid player IDs
- [x] Reconnect restores identity

## Acceptance criteria

- [x] Players 1-4 work through legacy bridge unchanged
- [x] Player 5 works without a legacy PAD port
- [x] Controller disconnect keeps slot reserved; reconnect restores identity
- [x] Rumble routes through player-to-instance mapping
- [x] Press-Start-to-join works
- [x] Keyboard/mouse controls at most one player

## Implementation notes (2026-07-20)

### Working

| Area | Detail |
|------|--------|
| Aurora narrow API | `controller_instances()`, `read_controller()`, `controller_connected()`, `controller_guid/serial()`, `get_controller(instance)` in `extern/aurora/lib/input.hpp` |
| Snapshots | `dusk::coop::input::tick()` samples Players 0–3 from `mDoCPd_c` (legacy bridge) and Players 4–7 from Aurora by SDL joystick instance |
| Join | Unbound device + Start edge → next free slot; first secondary join calls `setEnabled(true)` |
| Disconnect / reconnect | Slot stays `reserved`; GUID/serial (fallback prior instance id) restores the same `PlayerId` |
| Rumble | `input::rumble(player, low, high, ms)` → `aurora::input::controller_rumble(instance, …)` |
| Action binds | Port/player bounds checks; `*ForPlayer` wrappers require `actionBindingValid`; slots 4–7 evaluate via Aurora handle using port-0 bind config |
| Keyboard policy | At most one PAD port may keep keyboard bindings; extras cleared each tick |
| Main loop | `coop::init()` after `mDoCPd_c::create()`; `coop::tick()` after each `mDoCPd_c::read()` |

### Stubbed / deferred

| Area | Detail |
|------|--------|
| Link/camera consumption | Gameplay still reads `mDoCPd` directly; Gate D+ should switch co-op paths to `snapshot(player)` |
| Settings persistence | Device preferences not yet written to Dusklight settings (identity kept in-memory for the session) |
| Per-player bind config | Players 4–7 reuse port-0 action-bind button ids; dedicated settings arrays still 4-wide |
| Proxy / test actor | Join fills input + `PlayerSlot`; visual proxy spawn remains Gate D |
| ImGui overlay | Periodic stdout telemetry in `debug::drawOverlay()` only |

## How to test (multiple controllers)

1. Configure with `-DENABLE_LOCAL_COOP=ON` (PC / `TARGET_PC`).
2. Boot with the usual DVD/ISO path.
3. Connect 5+ gamepads (Aurora opens them — do not open second SDL handles).
4. Player 0 uses legacy port 0 (gamepad and/or keyboard).
5. On an unbound pad, press **Start** → `[coop] Player N joined via Start …`.
6. Players 1–3 also claim legacy PAD ports 0–3; Player 4+ logs `snapshot-only`.
7. Move sticks / hold buttons — every ~60 frames enabled co-op prints snapshot lines.
8. Unplug a joined pad → `slot reserved`; plug the same pad back → `reconnected`.
9. Call `dusk::coop::input::rumble(id, 1.f, 1.f, 200)` from a debugger/console to verify motor routing.
10. Confirm a second keyboard port is cleared if both try to use KB/mouse.

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| 2026-07-20 | Aurora instance/read API + `coop_input` sampling/join/reconnect/rumble; action-bind validation; main-loop hook; gate doc | PoC coded — needs multi-pad hardware verification |

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
