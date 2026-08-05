# Adversarial review — M1 (player replication)

Reviewer: deepseek-v4-flash (PI_MODEL=deepseek/deepseek-v4-flash-0731)
Branch: `net-coop`
Date: 2026-08-05

> **IMPORTANT — the branch moved under me.** When this review started, HEAD was
> `195093aaae` (last of the mandate's M1 list) with `extern/aurora` pinned at
> `abb0c3d` (fork `coop` branch). Mid-review, commit `6551bb53f1`
> ("revert: aurora to upstream pointer (6c4c27f9); drop 8-controller boot
> guard", authored 14:11) landed on `net-coop`. All runtime testing below was
> done against the M1 code at `195093aaae` (pinned aurora); the build verdict
> covers both states. `git reflog` confirms `6551bb53f1` was committed during
> this review session.

---

## 0. Build & run

### 0.1 Build (M1 code at `195093aaae`, aurora pinned `abb0c3d`) — PASS

`ninja` in `build/macos-default-relwithdebinfo` was already current; I confirmed
by touching `coop.cpp` and the audio/stub objects and rebuilding — the full game
target links cleanly (the pre-existing `Dusklight.app` binary and object files
predate my session). No new warnings from the M1 code.

### 0.2 Build (current HEAD `6551bb53f1`, aurora reverted to `6c4c27f9`) — **FAIL**

After the revert, `ninja` does **not** link:

```
ld: symbol(s) not found for architecture arm64:
  "_AIInitDMA", referenced from:
      JASDriver::initAI(void (*)()) in libJSystem_JAudio2.a[31](JASAiCtrl.cpp.o)
      JASDriver::updateDac() in libJSystem_JAudio2.a[31](JASAiCtrl.cpp.o)
```

I forced-rebuilt the audio + stub objects and the aurora libs (aurora libs
rebuilt 14:40–14:41 against `6c4c27f9`) to rule out staleness; the failure is
structural:

- `libs/JSystem/src/JAudio2/JASAiCtrl.cpp:82,131` call `AIInitDMA(uintptr_t, u32)`
  → C symbol `_AIInitDMA` (`libs/dolphin/include/dolphin/ai.h` wraps the
  declaration in `extern "C"`).
- The game's own stub is `void AIInitDMA(u32 start_addr, u32 length)`
  (`src/dusk/stubs.cpp:911`) — **wrong signature**. The visible header
  declaration under TARGET_PC is `AIInitDMA(uintptr_t, u32)`
  (`extern/aurora/include/dolphin/ai.h:21-23`), so the stub compiles as a new
  C++ overload: `nm` shows only `__Z9AIInitDMAjj` — never `_AIInitDMA`.
- With the pinned fork aurora (`abb0c3d`, "Buffers+"), aurora's OS lib exported
  `_AIInitDMA` and the link succeeded. With upstream `6c4c27f9`, nothing
  provides it (`nm` on the rebuilt `libaurora_os.a`: 0 hits).

So the revert (a) restores upstream aurora tracking but (b) exposes a latent
stub bug and leaves the branch **unbuildable**. This contradicts the claim in
`review-m1-glm-5.2.md` that "the build at HEAD succeeds with the upstream
aurora" — I rebuilt after the revert and it does not.

### 0.3 Selftest — PASS (both states)

`dusk_net_selftest` builds and runs at HEAD: "PASS: all checks succeeded" (v3
PlayerState round-trip, WireSize, host relay A→host→B / B→host→A, origin never
echoed, SessionEnd propagation, slot reuse).

### 0.4 Two-instance loopback — RUN, and it works

I ran two real game instances on 127.0.0.1 (aurora GUI on the user's Mac, RVZ
from the repo, `--cvar net.enabled=true net.role=host|client
net.hostPort=44770`), plus a third run with `--load-save 1` to get out of the
title demo:

- **Title screen**: host + client join (v3), both spawn a puppet ("puppet for
  player 1 active (pid 57)" / "puppet for player 0 active (pid 49)"), both send
  PlayerState at 60 Hz — but **no apply lines**. Root cause: the framework's
  demo gate `dEvt_control_c::moveApproval` (`src/d/d_event.cpp:1136`) returns 0
  for actors with `demoActorID == 0` that are not the demo's Pt1/Pt2, and
  `fopAc_Execute` (`src/f_op/f_op_actor.cpp:300`) then skips their execute
  entirely. Verified with lldb (breakpoint on `dusk::coop::puppetExecute`:
  zero hits on the title screen). This is the "frozen in cutscenes" behavior —
  implemented by the engine, even stricter than the design's demo-bit hold.
- **In-game** (`--load-save 1`, both instances same room): the apply path runs
  end-to-end. Client log: `apply player 0 to pos=(296.8,800.0,-941.5)
  recv=(296.8,800.0,-941.5) yaw=-32768 room=1` — applied position **exactly**
  equals the received position; the host mirrored the client's Link the same
  way. 60 Hz send + apply diagnostics confirmed (1 Hz throttled prints while
  frame counter runs 60/s).
- **Leave**: killed the client → host logged "player 1 left the session" →
  "player 1 left; despawning puppet" → "puppet for player 1 destroyed (pid 81)".
- **Host kill / SessionEnd**: client received "host ended the session
  (reason 0)" → "session ended; puppets cleared" → puppet destroyed. (So the
  shutdown-ordering hazard in M4 below did not bite on macOS in practice.)
- **Save integrity**: `USA/Card A/01-GZ2E-gczelda2.gci` mtime unchanged
  (Aug 1) across all runs — no host-save writes.

---

## 1. VERDICT

**M1 does NOT meet its acceptance criteria at the current HEAD**, for one hard
reason and three functional ones:

1. **HEAD does not build** (`6551bb53f1`, the aurora revert, breaks the link —
   §0.2). A milestone that cannot be built cannot be accepted.
2. **B1 — `setStartProcInit()` still runs in full for puppets** (the commit
   message claims it was "replaced by procWaitInit"; the code calls *both*).
   On horse-start stages this teleports the **host's horse** to the puppet's
   spawn point, force-rides the puppet onto it, and leaves both in a corrupted
   state. This is the plan's risk-3 family (host state corruption) not fully
   neutralized.
3. **3rd+ players are invisible to earlier joiners** — the roster is never
   rebroadcast on join.
4. **A mutual room-change sender-gate deadlock** can freeze both puppets
   forever when two players enter the same new room in the same frame.

The **core 2-player pose mirror works** — I verified it live (position-exact
mirror, 60 Hz, join/leave/session-end lifecycle, no save writes, selftest
green). But the acceptance criteria as written ("remote Link mirrors pose;
frozen in cutscenes; survives room changes; no host-save damage; single-player
untouched") are only partially met: mirror ✓, frozen-in-cutscenes ✓ (both the
demo-bit hold and the engine gate), room changes ✗ (deadlock risk + hidden
gap), no host-save damage ✓ (save writes guarded; world-state horse corruption
✗ on horse stages), single-player untouched ✓ (behavioral; one always-on
`modelCalc` deviation, MINOR m3).

**Verdict: BLOCK — fix BLOCKER A1 + B1 + the two MAJORs before M2.**

---

## 2. RANKED FINDINGS

### BLOCKER A1 — Current HEAD (`6551bb53f1`) does not link (`_AIInitDMA` undefined)

- **Claim:** The aurora revert commit leaves the branch unbuildable.
- **Evidence:** `ninja -C build/macos-default-relwithdebinfo` →
  `ld: symbol(s) not found for architecture arm64: "_AIInitDMA"` referenced
  from `JASDriver::initAI` / `JASDriver::updateDac` in
  `libJSystem_JAudio2.a(JASAiCtrl.cpp.o)`. `nm` on
  `build/.../extern/aurora/libaurora_os.a` (rebuilt 14:40 for `6c4c27f9`): 0
  AIInitDMA symbols. `nm src/dusk/stubs.cpp.o`: only `__Z9AIInitDMAjj` — the
  stub `src/dusk/stubs.cpp:911` (`void AIInitDMA(u32, u32)`) does not match the
  TARGET_PC C declaration `AIInitDMA(uintptr_t, u32)`
  (`extern/aurora/include/dolphin/ai.h:21-23`, inside `extern "C"`), so it
  never satisfies the `_AIInitDMA` reference. The pinned fork aurora
  (`abb0c3d`) previously provided `_AIInitDMA`, masking the bug.
- **Why it matters:** the branch as checked out cannot produce a binary; CI
  fails; M1 cannot be tested from HEAD; the revert was made on the
  (incorrect) belief that the build still passes.
- **Fix:** give the stub the exact declaration — `void AIInitDMA(uintptr_t
  start_addr, u32 length)` (or wrap in `extern "C"`) so it emits `_AIInitDMA`;
  verify boot. This keeps upstream aurora tracking *and* restores buildability.
  (Do not just re-pin the fork; the stub fix is one line and the fork pin
  carries the 8-controller fork debt.)

### BLOCKER B1 — `setStartProcInit()` runs fully for puppets → host horse repositioned + force-ride on horse stages

- **Claim:** the create path neutralizes story init for puppets.
- **Evidence:** `src/d/actor/d_a_alink.cpp:5128-5137`:
  ```cpp
  int midna_prm = setStartProcInit();
  #if TARGET_PC
      if (isPuppetCreate) {
          midna_prm = 0;
          procWaitInit();
      }
  #endif
  ```
  `setStartProcInit()` (d_a_alink.cpp:4705) runs in full for the puppet. Its
  horse branch: `if (isHorseStart) { horsep->setHorsePosAndAngle(&current.pos,
  shape_angle.y); horsep->initHorseMtx(); initForceRideHorse(); }` where
  `horsep = dComIfGp_getHorseActor()` — **the host's horse**. `last_mode == 8`
  → `initForceRideHorse(); procHorseComebackInit();`. `initForceRideHorse`
  (`src/d/actor/d_a_alink_horse.inc:178-185`) does
  `mRideAcKeep.setData(hostHorse); mRideStatus = RIDETYPE_HORSE;
  hostHorse->onRideFlg();`. `isHorseStart = checkHorseStart(getLastSceneMode(),
  getStartMode())` — `getLastSceneMode()` is host-global save; `getStartMode()`
  is 0 for puppets (gparam 0). The subsequent `procWaitInit()` resets
  `mProcID` but not `mRideStatus`/`mRideAcKeep`/`onRideFlg`/horse position.
- **Why it matters:** on Hyrule Field and any stage entered with
  `last_mode 1/8`, every puppet spawn teleports the host's horse to the puppet
  spawn point, flags it ridden, and binds the puppet to it — visible host
  world-state corruption, exactly the risk-3 family the plan says is guarded.
- **Fix:** skip `setStartProcInit()` entirely for puppets (`procWaitInit()`
  instead), and force `isHorseStart`/`checkCanoeStart()`/`checkBoarStart()` and
  the `startPoint == -4` portal search to false/off for puppets in the
  phase-2 wait block (d_a_alink.cpp:5068-5086).

### MAJOR M1 — Roster never rebroadcast on join → 3rd+ players invisible to earlier joiners

- **Claim:** "PlayerId↔peer mapping correct through the host relay" / 2–8
  players.
- **Evidence:** `Session::OnJoinRequest` (`src/dusk/net/session.cpp:272-336`)
  sends `JoinAccept` + `WorldInit` **only to the joining peer**
  (`SendToPeer`). No `SendToAll(WorldInit, except=newId)` or roster-refresh to
  already-joined clients. `RemovePlayer` *does* broadcast `PlayerLeave`
  (session.cpp:517-535) — join and leave are asymmetric. On the game side,
  `PumpSessionAndSpawns` spawns only when `roster[i].present` (coop.cpp), and
  `RemoteInOurRoom` skips non-present slots — so client A (joined first) never
  spawns a puppet for, and never sends to, later joiner B. B sees A (its
  JoinAccept/WorldInit carried the then-current roster); A never sees B.
  2-player works (my runtime test); 3+ is broken. The selftest misses it (its
  relay test joins A then B but never checks A's roster after B joins).
- **Fix:** on successful join, broadcast the updated roster to all joined
  peers — `SendToAll(WorldInit, current roster, exceptPlayer=newId)` — and
  document WorldInit as a roster-refresh message.

### MAJOR M2 — Mutual room-change sender-gate deadlock (both players in the same new room, no messages ever flow)

- **Claim:** "survives room changes".
- **Evidence:** `RemoteInOurRoom` (`src/dusk/coop/coop.cpp:282-296`):
  send iff some remote has `!slot.hasState || slot.state.roomNo == myRoom`.
  Two players entering the **same new room C together** each hold the other's
  stale state (room ≠ C) → both gates false → neither sends → both puppets stay
  hidden forever. No timeout/fallback exists (`ReceiveSlot::lastFrame` is
  written but never read), and the reliable `PlayerEvent(SceneChange)` is
  inside the same gated send path, so it cannot break the deadlock. (GLM's
  review noted the loose gate as a bandwidth nit; the deadlock is the sharper
  edge.)
- **Fix:** always send `SceneChange` regardless of the gate, or send when the
  local room changed, or make the gate "remote.room == myRoom **or** my room
  changed since the remote's last state".

### MAJOR M3 — A create stuck on `cPhs_INIT_e` locks `g_createInFlight` forever (no further puppets ever spawn)

- **Claim:** spawn lifecycle is self-healing.
- **Evidence:** `PumpSpawns`' Creating-state watchdog (coop.cpp) re-requests
  only when `fopAcM_SearchByID(e.pid) == nullptr`. But `daAlink_c::create()`'s
  phase-2 wait block (d_a_alink.cpp:5068-5086) can return `cPhs_INIT_e`
  **forever** without the process dying, e.g. for puppets:
  - ground check at the fixed +120-unit spawn offset fails over a cliff/void
    (`mLinkAcch.GetGroundH() == -G_CM3D_F_INF`);
  - `startPoint == -4 && !portalActor` (host entered via warp; portal gone);
  - `checkCanoeStart() && !searchCanoe`, `checkBoarStart() && !searchBoar`
    (host save start-point conditions with the ride actor already despawned);
  - `isHorseStart && dComIfGp_getHorseActor() == NULL`.
  A process spinning at `cPhs_INIT_e` keeps `g_createInFlight == true`
  (`onLinkCreated` never fires), so **every later spawn attempt for any player
  is blocked**.
- **Fix:** suppress the ride/portal/light-ball wait conditions for puppets
  (same guard family as B1) and/or add a create-phase deadline that deletes the
  request and re-requests.

### MAJOR M4 — Shutdown ordering: `enet_deinitialize` runs before the session teardown that sends SessionEnd/PlayerLeave

- **Claim:** graceful teardown wired correctly.
- **Evidence:** `src/dusk/config.cpp:645-649`:
  ```cpp
  dusk::net::shutdown();   // enet_deinitialize
  dusk::coop::shutdown();  // Session::Stop → sends SessionEnd/PlayerLeave via transport
  ```
  The comments claim the opposite ("graceful session stop **before** ENet
  tears down" — false). `Session::Stop` → `transport_.Stop()` joins the socket
  thread whose final `DrainOutbox` calls `enet_peer_send`/`enet_host_destroy`
  on a deinitialized ENet. It worked on macOS in my test (SessionEnd actually
  arrived — runtime evidence), but it is fragile; on Windows
  `enet_deinitialize` tears down Winsock (the M0 commit `2211c92814` calls this
  out).
- **Fix:** call `dusk::coop::shutdown()` before `dusk::net::shutdown()`.

### MINOR m1 — Stale wire-size comment: `2657` vs actual `2001`

`src/dusk/net/protocol.cpp:156` comments `// 2657 — raw-matrix pose (Rev 3
D4)`; `PlayerStateWireSize()` = `5+5+10+1+12+48+40·48` = **2001** (`sizeof(Mtx)
== 48`, 3×4). The value returned is correct; the comment assumes a 64-B Mtx.

### MINOR m2 — `modelCalc` mtxCalc re-assert runs for the real Link with networking off

`src/d/actor/d_a_alink.cpp:19489-19509` gates only on `#if TARGET_PC` and
`i_model == mpLinkModel`, not on session/puppet state — single-player now does
3 extra `setMtxCalc` writes per frame. Harmless (re-asserts the same pointers
`changeModelDataDirect*` already armed) but it is the one always-on deviation
from byte-for-byte vanilla. Gate on `remoteCount() > 0 || isPuppet(this)` or
document as intended.

### MINOR m3 — `PlayerEvent(SceneChange)` is sent but never consumed

The receiver's `ApplyPendingEvent` (coop.cpp) only handles `Equip`; SceneChange
is explicitly ignored (room rides in PlayerState). Dead reliable wire traffic;
fine as documented, but either consume it or don't send it.

### MINOR m4 — No staleness/timeout on receive (`seq` from 02-player-state.md §2 never shipped)

`PlayerStateMsg` has no `seq`; `ReceiveSlot::lastFrame` is written but never
read. A remote that dies without leaving the session leaves its puppet held in
the last pose forever — behaviorally the plan's "freeze last pose", but the
"seq-gap timeout → freeze/fade" mechanism is absent and 02-player-state.md §2
(`seq u16`) still documents it.

### MINOR m5 — Dead public API

`dusk::coop::hostRole()` / `sessionActive()` are exported (coop.h) but have no
callers outside coop.cpp. Trim or use in the M2/M4 UI.

### Note (not a finding) — initial-equip sync is actually covered

GLM's MAJOR M2 ("initial equip never synced to a freshly-spawned puppet") does
not hold: the change-trackers (`g_lastSentEquip = 0xFFFF` etc., coop.cpp)
initialize to sentinels, so the **first** gated send fires the full event burst
(form/equip/attention/scene), and the receive slot persists `hasEvent` until
the puppet's first apply — even if the puppet spawns later. Residual: a
1-frame host-equip flash at spawn. I verified the flow statically; no runtime
equip test was possible (no input-driven equip change).

---

## 3. VERIFIED-OK (claims checked and confirmed)

- **Build of the M1 code (pre-revert state)** and **selftest** — full game
  target links; `dusk_net_selftest` PASS (v3 round-trip incl. 40×Mtx + baseTR +
  scaleFlags + face; host relay; origin never echoes; version-mismatch,
  session-full, slot-reuse, SessionEnd).
- **Two-instance loopback (in-game)** — join (v3, player 1), WorldInit, mutual
  puppet spawn + Active, 60 Hz PlayerState both directions, **position-exact
  apply** (`apply player X to pos=recv`, both directions, room=1). See §0.4.
- **Frozen in cutscenes** — both the design's demo-bit hold
  (`stateFlags & kPlayerStateFlagDemo` early-return in `ApplyPuppetState`) and
  the engine's `dEvt_control_c::moveApproval` gate (d_event.cpp:1136 →
  f_op_actor.cpp:300 skip), which also froze the puppets on the title screen.
- **PlayerLeave despawn / SessionEnd clear** — runtime-verified
  ("despawning puppet" → "destroyed"; "session ended; puppets cleared").
- **No host-save damage** — every create-time save write guarded for puppets:
  `setRestartRoom` (d_a_alink.cpp:5082), `setSelectEquipClothes` ×4 (4924-4949),
  TKS-letter `setItem` (playerInit), `setTransformStatus` in `changeWolf`/
  `changeLink` (wolf.inc:221-228, 426-433), `setDamagePoint`/
  `setDamagePointNormal`/`setLandDamagePoint` (damage.inc:178-186, 237-241,
  243-249). Save file mtime unchanged across all test runs.
- **Slot-0 pointer clobber (risk 1)** — `setPlayer(0,this)`/`setLinkPlayer`
  immediately followed by the restore for puppets (d_a_alink.cpp:4958-4968);
  no slot-0 reads between; the block re-runs on every phase call (bgWaitFlg
  stays FALSE until loads complete), so the restore repeats each phase;
  destructor slot-0 clears guarded `if (!isPuppet(this))` (20024, 20068).
- **Fan-out suppression** — direct guards (not the fragile blacklist):
  MIDNA + `checkSetNpcTks` (5188-5201), CANOE/IceLeaf ride (5045-5058), portal
  `setPtD` (5082-5088), warp-miss tag + LV2 switches (5210-5221),
  light-ball `setForceGrab` (5176-5186), `setItemActor` (5157-5163).
- **Shared J3DModelData mtxCalc (risk 2)** — `modelCalc` re-asserts
  `field_0x1f20/1f24` on joints 0/1/16 (human) / 0/3/15 (wolf) before `calc()`
  for every Link (19489-19509), matching the fork's established indices.
- **Apply pipeline order** — `ApplyPuppetState` matches plan Rev 3 §5 M1:
  transform → form (`DriveFormSwap`, arc-on-demand + `changeWolf/changeLink`,
  hidden while loading) → `mProcID=PROC_WAIT` → face bck/btp → `setMatrix` →
  `allAnimePlay` → face frame + `playFaceTextureAnime` → `modelCalc` →
  per-joint `setAnmMtx` + `setScaleFlag` + `setBaseTRMtx` → `setItemMatrix`/
  `setWolfItemMatrix` → `setBodyPartPos` → `setAttentionPos` →
  `setCollisionPos`/`setWolfCollisionPos` → per-frame Tg registration ×3 with
  `SetMass` → `mLinkAcch.CrrPos` → `setRoomInfo`. No missing/reordered step.
- **Frame-interp (risk 12)** — pose writes go only through the public
  `J3DModel::setAnmMtx`/`setScaleFlag`/`setBaseTRMtx` (J3DMtxBuffer MTXCopy);
  no raw buffer writes. The dusk interpolator records/replaces matrices only in
  d_drawlist (camera/proj), d_a_midna, iron-ball chain, boot concat — never the
  Link anmMtx buffers — so nothing fights the applied pose.
- **Sender (02 §3)** — frame-final read at the execute tail (after
  setMatrix/modelCalc/setItemMatrix, d_a_alink.cpp:19036-19045); puppets never
  reach the sender (frozen branch first); `jointCount` capped at `kMaxJoints`,
  receiver clamps to `min(jointCount, localJoints)`; `DeserializePlayerState`
  rejects `jointCount > kMaxJoints`; the `195093aaae` baseTR diagnostic fix is
  correct (`baseTR[2][3]`, Mtx is 3×4).
- **60 Hz cadence (D2)** — one send per real-Link execute, no timer/batching;
  runtime-observed at 60 Hz.
- **Hidden state** — room mismatch → `entry.hidden` → pose work + draw
  skipped (`puppetDrawHidden`); fresh puppets spawn hidden until first state;
  the `stageOk` clause is currently inert (empty `worldStage_`, M4 fills it).
- **Star relay / duplicate-join guard** — host relays inbound
  PlayerState/PlayerEvent to all joined peers except origin (selftest-verified,
  incl. "origin never gets its own message back"); `OnJoinRequest` rejects a
  second JoinRequest from a peer that already holds a PlayerId
  (session.cpp:279-284); both carried into the game glue.
- **net.enabled=false gate** — `EnsureSession` returns, `SessionLive()` false →
  no session, no spawns, no sends; `isPuppet` false everywhere → all guarded
  sites run vanilla. Only deviation: MINOR m2 (`modelCalc`).
- **Aurora pin (7845c1282c), as reviewed in the mandate list** — it *was*
  necessary to build against the aurora the M0/M0.5 work used (the fork's
  `coop` branch, `PAD_CHANMAX=8`); it pinned a known-good-for-this-tree commit
  (builds, boots — my runtime evidence) but a fork-branch tip, not an upstream
  commit, which is exactly why it does not survive as a tracking pin. Its
  action_bindings 4–7 range fix was a genuine boot blocker on the 8-controller
  aurora. **Current state note:** the revert `6551bb53f1` supersedes it but
  breaks the build (BLOCKER A1) — the correct end state is upstream aurora +
  the one-line `AIInitDMA` stub fix, not the pin.

---

## 4. TOP MUST-FIX before M2

1. **Restore buildability at HEAD** — fix the `AIInitDMA` stub linkage
   (BLOCKER A1); verify boot + loopback again at the reverted aurora.
2. **B1** — skip `setStartProcInit()` for puppets (`procWaitInit()` instead)
   and suppress the horse/canoe/boar/portal phase-2 wait conditions; this also
   removes most of the stuck-create hazard (M3).
3. **Roster rebroadcast on join** — `SendToAll(WorldInit)` after a new join so
   3+ players see each other (MAJOR M1).
4. **Room-change sender gate** — never let stale room state silence both
   directions (MAJOR M2); at minimum always send `SceneChange`.
5. **Stuck-create deadline** — release `g_createInFlight` on a create that
   spins (MAJOR M3).
6. **Shutdown order** — `coop::shutdown()` before `net::shutdown()`
   (MAJOR M4).
7. **Docs** — update 00-network.md §5/§6 to the v3 raw-matrix wire (see §5).

---

## 5. Spec drift vs 00-network.md

| Area | Doc (00-network.md) | Impl (v3 wire) | Recommendation |
|------|---------------------|----------------|----------------|
| PlayerState payload | §5 joint-list: `scene u8, movementFlags u8, cosmetics[3], itemAction s8, invincibility u8, stateFlags u32, pos, rot s16×3, upperLimbRot s16×3, joints Vec3s16[40]` ≈ 279 B | `playerId, roomNo s8, form, stateFlags u8, jointCount, scaleFlags[5], yaw/pitch s16, faceBckIdx/faceBtpIdx u16, faceFrame s16, reserved, pos, baseTR Mtx, joints Mtx[40]` ≈ 2001 B | Rewrite §5 to the `PlayerStateMsg` struct; make `protocol.h` normative (the doc already claims it is) |
| PlayerEvent | §5 "player id + event + data" | + `scene u8, reserved u8, data2 u32` (item joints) | Document `data2`/`scene` |
| Bandwidth | §6 "~2.7 KB" (4×4 Mtx assumption) | ~2.0 KB (3×4 Mtx = 48 B) | Correct §6 figure |
| `seq` | 02-player-state.md §2: `seq u16` | none | Reconcile 02 ↔ 00; implement or drop |
| WorldInit | §4 "sent right after JoinAccept" | sent to the joining peer only; no rebroadcast on later joins | Document WorldInit as roster-refresh and broadcast (MAJOR M1) |
| Host world stage | plan: "M1+ fills real values" | empty (`stage[0]='\0'`); `stageOk` inert | M4 TODO comment |
| Doc-vs-impl drift note in protocol.h | "only the serializer body and this struct change" | the §5 doc was **not** updated at the v3 bump | Update the doc in the same commit as the next wire change |

No drift in: envelope (`u16 type + u16 size`), channel split, star relay
topology, JoinRequest version gate, fixed-size roster, `kMaxJoints=40` /
`kMaxLocalPlayers=8`.

---

## 6. Notes on the concurrent review (`review-m1-glm-5.2.md`)

I reviewed the same milestone independently and differ on three points:

1. **Build at HEAD**: GLM reports "build OK" at `6551bb53f1`; my forced rebuild
   of the audio/stub objects and the aurora libs at that commit fails with the
   `_AIInitDMA` link error. The revert is build-breaking.
2. **Initial-equip sync**: GLM rates "initial equip never synced" MAJOR; I
   refute it (sentinel-initialized trackers + event persistence in the receive
   slot — see the note after the MINORs).
3. **New findings here**: the mutual room-change sender-gate deadlock (M2),
   the stuck-create `g_createInFlight` lock (M3), and the shutdown-ordering
   hazard (M4) are not covered by GLM.

Agreed findings: B1 (`setStartProcInit` horse corruption — GLM BLOCKER,
confirmed with `initForceRideHorse` evidence), the roster-not-rebroadcast
3-player bug, and the §5 doc drift.
