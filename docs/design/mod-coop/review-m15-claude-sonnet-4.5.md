# Adversarial review — M1.5 (M1 fix pass, player replication)

Reviewer: claude-sonnet-4.5
Branch: `net-coop`
Head reviewed: `9aff5a879a` ("M1.5 complete …")
Range: `6551bb53f1..9aff5a879a` (10 commits: `1d80fded5d` AIInitDMA stub,
`0b7f8bb5d1` alink create neutralization, `5d2cbe6be0` roster broadcast,
`74d5a1dce2` room-change gate + create deadline, `46409fba68` shutdown order,
`62dfbec6e2` relay-policy seam, `647a91f49c`+`5d244af7b2`+`d901f6a72b` docs,
`9aff5a879a` complete)
Date: 2026-08-05

> `extern/aurora` is intentionally at the upstream pointer `6c4c27f9` — this is
> the correct end state (per the M1 reviews) and is **not** flagged as a
> regression. The M1 BLOCKER A1 was precisely that the upstream pin exposed a
> latent stub bug; M1.5 fixes the stub so the upstream pin survives.

---

## 0. Build / run

### 0.1 Forced rebuild of the touched TUs + aurora_os — PASS

The previous review proved incremental builds can mask the A1 link failure, so
I force-rebuilt the three objects that matter for A1 before trusting anything:

```
rm CMakeFiles/dusklight.dir/src/dusk/stubs.cpp.o
rm CMakeFiles/JSystem_JAudio2.dir/libs/JSystem/src/JAudio2/JASAiCtrl.cpp.o
rm extern/aurora/libaurora_os.a
ninja -C build/macos-default-relwithdebinfo dusklight
```

Result: links clean (`[5/6] Linking CXX executable
Dusklight.app/Contents/MacOS/Dusklight`); no `_AIInitDMA` undefined. Only the
pre-existing `ld: warning: ignoring duplicate libraries` chatter.

Symbol evidence (`nm` on the freshly-rebuilt objects):

| object | `_AIInitDMA` |
|--------|-------------|
| `stubs.cpp.o` | `0000000000001a98 T _AIInitDMA` — **defined**, C linkage (no mangling) |
| `JASAiCtrl.cpp.o` | `_AIInitDMA` (undefined `U`) — the unresolved ref is now satisfied |
| `libaurora_os.a` | (none — the stub provides it; aurora's OS lib does not) |
| `Dusklight` (final exe) | `0000000100011c6c t _AIInitDMA` — resolved |

The full `touch`-all-touched-TUs rebuild (`stubs.cpp`, `d_a_alink.cpp`,
`coop.cpp`, `session.cpp`, `protocol.cpp`, `config.cpp`, `selftest_main.cpp`)
completes with **exit 0**; the only warnings are the same pre-existing
JSystem-header ones the M1 reviews already catalogued (`JKRExpExpHeap`
multi-char, `J3DStruct` nontrivial-memcall, deprecated `iterator<>`,
`daObjTks_c::setExpression` undefined-inline). **No new warnings from any M1.5
TU.** `mods/*.dusk` packages build clean.

### 0.2 Selftest — PASS

`./dusk_net_selftest` → **"PASS: all checks succeeded"**. The M1.5 roster
assertions (`RunGameMessageDemo`, `selftest_main.cpp:739-747`) fire and pass:

```
ok: host roster has 3 players
ok: A's roster shows B after B joined (WorldInit roster-refresh)
ok: B's roster shows A and the host
ok: A's roster has no phantom players beyond the roster
```

…plus the full v3 PlayerState round-trip, `WireSize == PlayerStateWireSize()`,
host relay A→host→B / B→host→A, "A never receives its own PlayerState back",
version/session-full/slot-reuse, and SessionEnd propagation.

### 0.3 Boot — starts clean (A1 did not break boot)

A time-boxed single-instance launch (`Dusklight --cvar net.enabled=false`, 12s)
logs the boot path without crashing:

```
[info] [dusk::net::module] enet initialized (ENet 1.3.18)
[INFO | dusk] Build: v1.4.1-135 (rev 9aff5a879a…, RelWithDebInfo)
[INFO | dusk] Platform: macos
[INFO | dusk::config] Loading config from '…/Dusklight/config.json'
```

aurora + ENet + config all init against the upstream aurora `6c4c27f9`. (A1 is
a pure linkage fix — the stub body is an unchanged no-op `STUB_LOG()`, so it
carries no runtime behavior change; boot was already verified end-to-end in the
M1 reviews with the same stub mechanism.)

### 0.4 Two-instance loopback — NOT re-run (environment)

A full two-instance loopback (two GUI windows, the RVZ, two config dirs/ports on
one machine) is not feasible from this headless agent session — the same
constraint GLM's M1 review hit. There **is** a `WindowServer`, the repo RVZ
(`Legend of Zelda, The - Twilight Princess (USA).rvz`), and a `USA/Card A` save
on this Mac, but the agent process has no interactive GUI session to drive two
windows. I verified boot (§0.3) but did not exercise pose mirror / join-leave /
save integrity live. The M1.5 changes preserve that behavior by construction
(see §3): A1 is runtime-neutral, B1/M2/M3 touch only the puppet/relay paths
(selftest + static-traced), M4 is shutdown-only, and the M1-verified pose-mirror
+ leave + no-save-write paths are unchanged outside the fixed sites.

---

## 1. VERDICT

**M1.5 resolves the M1 blockers and majors; HEAD is buildable and
regression-clean.**

| M1 finding | Severity | Resolved? | How (evidence) |
|------------|----------|-----------|-----------------|
| A1 HEAD unbuildable (`_AIInitDMA`) | BLOCKER | **YES** | stub now `AIInitDMA(uintptr_t,u32)` matching `extern/aurora/include/dolphin/ai.h:21` `extern "C"` decl → emits `_AIInitDMA` (`nm`: `T _AIInitDMA`); forced rebuild links; aurora stays on `6c4c27f9` (`1d80fded5d`) |
| B1 `setStartProcInit()` ran for puppets | BLOCKER | **YES** | puppets take the `else`-free `if (isPuppetCreate) { procWaitInit(); }` branch — `setStartProcInit()` is **not** called (`d_a_alink.cpp:5140-5155`); horse/canoe/boar/portal phase-2 waits guarded by `puppetSkipEntryWait` (`:5080-5086`); real Link path byte-identical (`else { setStartProcInit(); }`) (`0b7f8bb5d1`) |
| M1 roster not rebroadcast on join | MAJOR | **YES** | `Session::OnJoinRequest` → `SendToAll(WorldInit, init, exceptPlayer=id)` (`session.cpp:377`); selftest asserts A's roster post-B-join (`0b7f8bb5d1`+`5d2cbe6be0`) |
| M2 room-change sender deadlock | MAJOR | **YES** | `SceneChange` sent before the gate in `sendPlayerState` (`coop.cpp:871-877`); `RemoteInOurRoom` opens on `myRoomAtLastRecv != myRoom` (`:319-321`); traced both-direction send (`74d5a1dce2`) |
| M3 stuck-create locks `g_createInFlight` | MAJOR | **YES** | `kCreateDeadlineFrames=600` watchdog deletes the stuck process (`coop.cpp:585-597`); `fopAcM_delete` reaches the deletor on a stuck-creating process (`is_doing` is only true mid-handler; `init_state==1≠3`; daAlink `is_delete_method` is NULL→1) so `~daAlink_c`→`onLinkDestroyed` releases the lock; puppets only (`74d5a1dce2`) |
| M4 shutdown order | MAJOR | **YES** | `config.cpp:651` calls `dusk::coop::shutdown()` **before** `dusk::net::shutdown()`; stale comment rewritten (`46409fba68`) |
| Doc drift §5/§6 | MAJOR | **YES** | 00-network.md §5 rewritten to `PlayerStateMsg`/`PlayerEventMsg` (field-for-field == `protocol.h:261-289`); §6 ~2.0 KB / ~1.0 MB/s; `WorldInit` documented as roster-refresh (`647a91f49c`) |
| `seq` (02 §2) | MINOR | **YES** | dropped from §2; stale policy "last pose held, no fade" documented in §2 note + §8 (`647a91f49c`) |
| Minor a/b/d/e/f | MINOR | **YES** | wire-size comment 2001; `modelCalc` gated on `remoteCount()>0‖isPuppet`; `RelayPolicy` seam; `lastFrame`→`myRoomAtLastRecv`; `hostRole/sessionActive` retained+documented (`62dfbec6e2`, comments) |

**Verdict: APPROVE — no MUST-FIX before M2.** Residuals are MINOR (§2) and do
not block M2.

---

## 2. RANKED FINDINGS

### MINOR r1 — Light-ball phase-2 wait not suppressed for puppets (B1/M3 residual)

- **Where:** `src/d/actor/d_a_alink.cpp:5086`
  `|| ((checkCarryStartLightBallA() || checkCarryStartLightBallB()) && !fopAcIt_Judge((fopAcIt_JudgeFunc)daAlink_searchLightBall, NULL))`
- **Evidence:** `checkCarryStartLightBallA/B()` (`src/d/actor/d_a_player.cpp:517,529`)
  read `dComIfGs_getLastSceneMode()` (host save) gated on LV8 / Palace of
  Twilight. On such a stage entered with `lastSceneMode & 0x100000`/`0x80000`,
  a puppet's create waits on `daAlink_searchLightBall` for an actor that does
  not exist for the puppet → returns `cPhs_INIT_e` forever. The M1.5 fix guarded
  the horse/canoe/boar/portal waits with `puppetSkipEntryWait` but **not** this
  light-ball wait. (The post-wait `setForceGrab` side effect at `:5194` **is**
  guarded `if (!isPuppetCreate)` — so no story-proc/host-state corruption; the
  residual is purely a stuck-create hazard.)
- **Mitigated by M3:** the 600-frame deadline catches it (deletes + retries/drops).
  So this cannot deadlock `g_createInFlight` (verified in §3.5). The puppet
  just fails to spawn on that specific LV8 entry until the player leaves/rejoins
  (after 3 strikes) — degraded but safe.
- **Fix (optional, M2-cleanup):** add `!puppetSkipEntryWait &&` to the light-ball
  clause, matching the other four. One-line; closes the last B1/M3 host-save-read
  wait. Not a M2 blocker (M3 deadline backstops it).

### MINOR r2 — M3 strike semantics are delete-timing-dependent + stale comments

- **Where:** `src/dusk/coop/coop.cpp:84-86` (`CreateLimiter`), `:585-597`
  (deadline branch).
- **Evidence:** the deadline branch re-fires every frame while the stuck process
  is alive (`g_frameCount - createStartFrame >= 600` stays true), calling
  `fopAcM_delete(e.pid)` and `++deadlineHits` each frame. With an **async**
  delete (process dies over a few frames), 3 strikes accumulate in **3
  consecutive frames** → `dropped=true` and the `retryFrame=+30` re-request path
  is never reached (the `SearchByID==nullptr → Requested` branch fires only after
  the process dies, by which time `dropped` is set). With a **synchronous**
  delete, 1 strike per stuck incident → the `+30` re-request retries, and 3
  separate incidents drop. The comment ("after `kCreateDeadlineStrikes`
  consecutive aborts…") reads as "3 separate stuck incidents" but the async
  path is "3 frames past the deadline." Both release the lock and terminate
  the spawn (no infinite loop), so M3's core property holds — the difference is
  only retry-vs-immediate-drop.
- **Also:** `CreateLimiter.dropped` is a `bool` but its comment says
  `"0xFFFFFFFF suppression window"` — stale leftover from a frame-counted design.
- **Fix (cosmetic):** either (a) set `e.state = Requested; g_createInFlight =
  false` explicitly inside the deadline branch (don't rely on the async
  destructor for the lock — makes the strike count match "incidents"), or (b)
  fix the comments: drop "consecutive aborts"→"3 frames past the deadline" and
  replace the `0xFFFFFFFF` comment with `// sticky: cleared on leave/success`.

### MINOR r3 — 02-player-state.md §2 table per-field sizes still 64-B (4×4 Mtx)

- **Where:** `docs/design/mod-coop/02-player-state.md:155-166` — the §2 field
  table lists `baseTR 64` and `joint[0..39] 40×64`.
- **Evidence:** TP's `Mtx` is `f32[3][4] = 48 B` (`libs/dolphin/include/dolphin/mtx.h`,
  confirmed by `PlayerStateWireSize()` = `5+5+10+1+12+48+1920 = 2001`). The §2
  **TOTAL** row and the §2 reconciliation note were corrected to "~2.0 KB /
  48-B 3×4 Mtx", but the two per-field size cells (`64`, `40×64`) were not —
  the table is internally inconsistent (cells sum to ~2.67 KB, total says ~2.0
  KB). `00-network.md §5` is correct (48 B); only the 02 §2 table lags.
- **Fix:** `baseTR 64`→`48`, `40×64`→`40×48` in the §2 table.

### MINOR r4 — `protocol.cpp` wire-size comment arithmetic is loose (2000 vs 2001)

- **Where:** `src/dusk/net/protocol.cpp:156-161`
  `"5 + 5 + 10 (yaw/pitch face bck/btp/frame/reserved) + 12 + sizeof(Mtx) +
  40*sizeof(Mtx) = 2001"`.
- **Evidence:** the parenthetical lists "reserved" inside the "10" but the 5
  s16/u16 fields are 10 bytes and `reserved` is the separate `+1`; summing the
  comment's terms literally gives `5+5+10+12+48+1920 = 2000`, yet it claims
  `2001`. The **value** is correct — `PlayerStateWireSize()` (`protocol.h:549`)
  is `5 + 5 + 10 + 1 + 12 + sizeof(Mtx) + 40*sizeof(Mtx)` = `2001` (the `+1` for
  `reserved` is explicit there) and the selftest verifies it. Only the
  comment's intermediate grouping is mislabeled.
- **Fix:** write the `+1` explicitly: `5 + 5 + 10 + 1 (reserved) + 12 +
  sizeof(Mtx) + 40*sizeof(Mtx) = 2001`.

### MINOR r5 — `PlayerEvent(SceneChange)` received but not consumed (carried from M1)

- **Where:** `coop.cpp:362-371` (`ApplyPendingEvent` default case) + `:871-877`
  (sender). The M2 fix sends `SceneChange` reliably on room change **before**
  the gate, but the receiver discards it (room rides in `PlayerState.roomNo`,
  updated when the matching PlayerState lands — `:216`). This is intended
  (documented in the default-case comment) and is the M2 deadlock-breaker's
  reliable backup. It is dead-reliable-wire traffic (one event per room
  transition, not per-frame) — harmless and documented. Carried from M1 MINOR
  m3; noting it is still intentional, not a regression.

---

## 3. VERIFIED-OK (claims checked and confirmed)

### A1 — `_AIInitDMA` linkage (BLOCKER)

- `src/dusk/stubs.cpp:916` is now `void AIInitDMA(uintptr_t start_addr, u32 length)`
  matching the TARGET_PC declaration in `extern/aurora/include/dolphin/ai.h:21`
  (inside `extern "C"`). `nm` on the rebuilt `stubs.cpp.o`: `T _AIInitDMA`
  (C symbol, no `__Z` mangling — the old `void AIInitDMA(u32,u32)` emitted only
  `__Z9AIInitDMAjj`). `JASAiCtrl.cpp.o` references `_AIInitDMA` (undefined);
  `libaurora_os.a` provides nothing (the stub satisfies the ref); the final
  `Dusklight` exe resolves it. **Forced-rebuild links clean; boot inits.** aurora
  remains on upstream `6c4c27f9` (`git submodule status`).

### B1 — puppet create neutralization (BLOCKER)

- `d_a_alink.cpp:5140-5155`: `int midna_prm;` then `#if TARGET_PC
  if (isPuppetCreate) { midna_prm = 0; procWaitInit(); } else #endif {
  midna_prm = setStartProcInit(); }`. Puppets call `procWaitInit()` **instead**
  of `setStartProcInit()` (the latter is in the `else` branch — unreachable for
  puppets). No horse reposition, no `initForceRideHorse`, no `mDemo`/`proc*Init`
  story procs for puppets.
- Phase-2 wait (`:5078-5086`): `puppetSkipEntryWait` (= `isPuppetCreate` under
  TARGET_PC, `FALSE` otherwise) guards the portal search (`startPoint == -4`),
  `checkCanoeStart()`, `checkBoarStart()`, and `isHorseStart` waits → a puppet
  create cannot spin on or bind to the host's horse/canoe/boar/portal. (Residual:
  the light-ball wait — MINOR r1 — is backstopped by M3.)
- Real-Link path byte-identical: the `else` calls `setStartProcInit()` exactly as
  before; `setSelectEquipItem/setMatrix/allAnimePlay/...` unchanged. The
  `puppetSkipEntryWait = FALSE` non-PC branch leaves every wait condition
  vanilla (guard hygiene).

### M1 — roster rebroadcast on join (MAJOR)

- `session.cpp:349-377`: after `AssignPlayerId` + `SendToPeer(JoinAccept)` +
  `SendToPeer(WorldInit)` to the joiner, the host calls
  `SendToAll(MsgType::WorldInit, init, exceptPlayer=id)`. `SendToAll`
  (`:520-528`) iterates player ids, skips `exceptPlayer` and non-present, maps
  each to its peer via `PlayerPeer`, sends. The new player (`id`) is excluded
  (already got its own WorldInit); the host's own slot (`PlayerPeer(0)` →
  `kInvalidPlayerId` on the host — no peer for the local player) is skipped →
  **no self-echo, no feedback, no double-send to the joiner.**
- Game-side no-churn: `PumpSessionAndSpawns` (`coop.cpp:695-712`) only transitions
  `None → Requested` for newly-present slots; an already-`Active`/`Creating`
  puppet is **not** touched (no duplicate, no respawn). A re-received WorldInit
  that re-affirms an already-present host just re-runs `ApplyRoster` (idempotent
  `present=true`) → no spawn churn. `Selftest` asserts 3-player correctness:
  A's roster shows B; B's shows A + host; no phantom slot
  (`selftest_main.cpp:739-747`, all PASS).

### M2 — room-change sender-gate deadlock (MAJOR)

- Sender (`coop.cpp:871-877`): `roomNow != g_lastSentRoom` → send
  `PlayerEvent(SceneChange)` **before** the `RemoteInOurRoom` gate (reliable, one
  event per room transition — not per-frame). So a room change always propagates
  even when the gate is shut.
- Gate (`coop.cpp:305-322`): `RemoteInOurRoom` returns true if `!slot.hasState`
  || `slot.state.roomNo == myRoom` || **`slot.myRoomAtLastRecv != myRoom`** (our
  room changed since the remote last sent us state). `myRoomAtLastRecv`
  (`:107`, init `-1`) is set to `LocalRoomNo()` on each PlayerState receive
  (`:216`); `LocalRoomNo()` (`:177`) = `fopAcM_GetRoomNo(g_realLink)`, same source
  as `myRoom` in the gate.
- Trace (A & B enter new room C same frame): both moved → each one's
  `myRoomAtLastRecv` (their room at the other's last send = old room) ≠ their
  new `myRoom` (C) → both gates open → both send `PlayerState(roomNo=C)` → each
  receives the other's C-state → `slot.state.roomNo == C == myRoom` next frame
  → stable. The reliable `SceneChange` is the backup prompt. **No path leaves
  both gates shut forever** (the only stale-room-stuck case requires
  `myRoomAtLastRecv == myRoom` AND `state.roomNo != myRoom` persistently, which
  needs receiving a state *while in C* with `roomNo != C` — but the first
  in-C exchange opens the gate and delivers `roomNo=C`). `lastFrame` (dead,
  written-never-read in M1) replaced by the read `myRoomAtLastRecv`.

### M3 — create-phase deadline (MAJOR)

- Deadline (`coop.cpp:84-86,585-597`): `kCreateDeadlineFrames=600` (10 s @ 60 Hz),
  `kCreateDeadlineStrikes=3`. In `PumpSpawns`' Creating branch, if
  `fopAcM_SearchByID(e.pid)` is null → re-request (process died); else if
  `g_frameCount - e.createStartFrame >= 600` → `fopAcM_delete(e.pid)`,
  `++deadlineHits`, drop after 3. `createStartFrame` set at issue (`:669`).
- **Lock release verified:** `fopAcM_delete` → `fpcM_Delete` → `fpcDt_Delete`
  (`f_pc_deletor.cpp:107`). The two early-returns there — `fpcCt_IsDoing`
  (`is_doing`, set true only *synchronously* inside `fpcCtRq_Do`'s
  `pHandler(i_request)` call, `f_pc_create_req.cpp:83-86`) and
  `state.init_state == 3` (a stuck-`cPhs_INIT_e` process has `init_state == 1`
  from `fpcBs_SubCreate`, `f_pc_base.cpp:188`) — **do not** block a delete that
  runs between frames (PumpSpawns' context). daAlink's `is_delete_method` is
  `NULL` (`l_daAlink_Method`, `d_a_alink.cpp:20124`) → `fpcMtd_Method(NULL,…)=1`
  (deletable) → `fpcDt_ToQueue` queues it → `fpcDt_Handler` → `fpcBs_Delete` →
  `~daAlink_c` → `onLinkDestroyed` (`coop.cpp:837-845`):
  `g_puppets[pid] = PuppetEntry{}; g_createInFlight = false;`. **Lock released.**
- **Puppets only, not the real Link:** the deadline runs only over
  `g_puppets[i].state == Creating`; the real Link is tracked by `g_realLink` /
  `g_realLinkReady` (not in `g_puppets`), and the puppet issue loop waits for
  `g_realLinkReady` before issuing (`:639-642`), so `createStartFrame` is set
  only after the real Link's create completed. A legitimately-long real-Link
  load is never deadline-killed; a puppet's phase-1 (shared, already-loaded arc)
  is sub-second, so 600 frames is generous and never false-aborts a normal
  puppet. `onLinkCreated` (`:822`) and leave (`:702`) reset
  `g_createLimiter[i]` → a rejoin restarts the budget.

### M4 — shutdown order (MAJOR)

- `config.cpp:651`: `dusk::coop::shutdown();` then `dusk::net::shutdown();`
  (coop **before** net). The old misleading comment ("graceful session stop
  before ENet tears down" placed *after* `net::shutdown`) is replaced by a
  correct explanation of the ordering hazard. `coop::shutdown` → `Session::Stop`
  sends PlayerLeave/SessionEnd via the still-alive transport, **then**
  `net::shutdown` tears down ENet.

### Relay-policy seam (MINOR d)

- `session.cpp:27-58`: `enum RelayPolicy { None, Star }`; `PolicyFor` returns
  `Star` for `PlayerState`/`PlayerEvent`, `None` otherwise.
  `ForwardGameMessage` (`:479-492`) early-returns unless `PolicyFor(type) ==
  Star`, then relays to every joined peer (with a PlayerId) except origin.
  **Behavior-preserving for M1** — `HandleData` (`:272-294`) only routes
  `PlayerState`/`PlayerEvent` through `ForwardGameMessage` (both `Star`);
  handshake types are dispatched directly; M2 types hit the `default` (ignored).
  The seam makes M2's `CombatIntent` (no-relay) / `EnemySnapshot`
  (owner→clients) expressible as `None` without touching the Star fallthrough —
  **no behavior change slipped in.**

### Docs sync (MAJOR M3 / MINOR)

- `00-network.md §5` `PlayerState` block matches `protocol.h:261-277`
  **field-for-field, wire order, sizes**: `playerId u8, roomNo s8, form u8,
  stateFlags u8, jointCount u8, scaleFlags u8[5], yaw s16, pitch s16, faceBckIdx
  u16, faceBtpIdx u16, faceFrame s16, reserved u8, pos f32×3, baseTR Mtx,
  joints Mtx[40]` = 2001 B (48-B `Mtx`). `PlayerEvent` block matches
  `protocol.h:279-285` (`playerId, eventId, scene, reserved, data u32, data2
  u32` = 12 B). `stateFlags` bit table (§5) matches `protocol.h:249-253`
  (bits 0-4) + documented bit-5 no-draw (never on wire).
- `00-network.md §6`: "~2.0 KB (48-B 3×4 Mtx)"; "~1.0 MB/s worst case (8 ×
  2001 B × 60 ≈ 0.96 MB/s players + 40 × 28 B × 60 ≈ 67 KB/s enemies)" —
  arithmetic checks out.
- `00-network.md §4` `WorldInit` documented as roster-refresh broadcast on every
  later join (the M1 fix).
- `seq`: dropped from `02-player-state.md §2` (field table + §2 note +
  §8/§9 Q8 "stale = last pose held, no fade; seq-gap timeout deferred"); 00 §5
  has no `seq` — consistent. `ReceiveSlot::lastFrame` (M1 dead write) replaced
  by the read `myRoomAtLastRecv`.

### Conventions / scope

- All vanilla-file changes are `#if TARGET_PC` guarded (`d_a_alink.cpp` puppet
  branch, `puppetSkipEntryWait`, `modelCalc` gate) with `#else FALSE` /
  byte-identical non-PC paths; net/coop changes are in the dusk-owned layer
  (`session.cpp`, `coop.cpp`, `protocol.cpp`, `config.cpp`, `stubs.cpp`).
- **No M2 features slipped in:** `EnemySnapshot`/`CombatIntent`/`EnemyEvent`
  structs exist in `protocol.h` but hit `HandleData`'s `default` (ignored); the
  `RelayPolicy` seam is "seam only, not wired" (its own comment). `worldStage_`
  is still empty / `stageOk` still inert — correctly deferred to M4 (commented).
- Commit hygiene: one commit per finding + docs, logical ordering, no
  force-pushes in the range. No source files modified by this review.
- `hostRole()`/`sessionActive()` retained + documented as M4-host-leave/
  discovery UI (`coop.cpp:769-770`); `selfId()`/`remoteCount()`/`isPuppet()`
  are used (sender + `modelCalc` gate). No new dead code beyond the documented
  M4-retention.

---

## 4. MUST-FIX before M2

**None.** M1.5 resolves both M1 BLOCKERs (A1, B1) and all four MAJORs (M1
roster, M2 room-deadlock, M3 stuck-create, M4 shutdown) plus the doc drift and
the tracked MINORs. HEAD builds clean under a forced rebuild of every touched
TU, the selftest passes (including the new 3-player roster assertions), and boot
inits against the upstream aurora. The two-instance loopback was not re-run
(headless agent) but is preserved by construction + selftest + the M1 runtime
evidence.

The §2 MINORs (light-ball wait, M3 strike comments, 02 §2 table sizes,
protocol.cpp comment arithmetic) are cosmetic/optional cleanups — r1 is the
only one with behavioral teeth and the M3 deadline already backstops it; safe
to fold into an M2-cleanup commit or leave for a playtest-driven polish pass.
