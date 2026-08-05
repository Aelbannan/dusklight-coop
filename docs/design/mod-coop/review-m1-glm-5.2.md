# Adversarial review — M1 (player replication)

Reviewer: glm-5.2 (PI_MODEL=z-ai/glm-5.2)
Branch: `net-coop`
Head reviewed: `6551bb53f1` (NOTE: this is one commit past the mandate's M1 list —
see MINOR-m6)
Date: 2026-08-05

## 0. Build / run

- **Build: OK.** `cmake --build build/macos-default-relwithdebinfo` (Ninja) builds
  the game cleanly after `touch src/dusk/coop/coop.cpp src/d/actor/d_a_alink.cpp`.
  Only pre-existing warnings (`daObjTks_c::setExpression` undefined-inline, etc.);
  no new warnings/errors from the M1 code. `mods/*.so` packages link fine.
- **Selftest: PASS.** `./dusk_net_selftest` → "PASS: all checks succeeded".
  Covers v3 PlayerState round-trip (incl. full 40×Mtx joint table + baseTR +
  scaleFlags + face), `WireSize(PlayerState) == PlayerStateWireSize()`, and the
  new host-relay test (A→host→B PlayerState, B→host→A PlayerEvent, origin never
  receives its own send, host broadcast reaches both clients).
- **Two-instance loopback run: NOT performed.** Running two game instances needs
  a macOS display/window, the mounted RVZ, and two separate config dirs/ports on
  one machine — not feasible in this CLI environment. Review is otherwise static
  against the source at HEAD; the selftest validates the protocol/session/relay
  layer that the loopback test would exercise end-to-end.

## 1. VERDICT

**M1 does NOT cleanly meet its acceptance criteria as shipped.** The acceptance
bar is: *remote Link mirrors pose; frozen in cutscenes; survives room changes;
no host-save damage; single-player untouched; disable/reconnect clean.*

- Pose mirroring, cutscene freeze, room-change survival, single-player
  behavioral vanilla-ness, disable/reconnect, and the protocol/relay plumbing
  are **sound** (see VERIFIED-OK).
- **No host-save damage**: the explicit save-write paths (setRestartRoom,
  setSelectEquipClothes, setDamagePoint, setTransformStatus, TKS letter) are all
  guarded — **OK for save**. But the create path still runs
  `setStartProcInit()` in full for puppets (BLOCKER B1), which repositions the
  **host's horse** and force-rides the puppet onto it on horse stages — that is
  host *world-state* corruption (not save, but in-scope for "no host-save
  damage" in spirit and for playability).
- The commit message of `9277ad4521` claims "setStartProcInit replaced by
  procWaitInit"; the code calls **both** (`setStartProcInit()` runs, then
  `procWaitInit()` resets `mProcID`). The replacement described in the plan
  (Rev 3 §5 M1 / 01-puppet-link §6.2) was not actually implemented.
- 2-player LAN almost certainly works for the pose-mirroring demo. 3+ players
  do not (MAJOR M1: roster is never rebroadcast to existing clients on join, so
  earlier clients never spawn a puppet for a later joiner).

**Verdict: BLOCK — fix B1 + M1 before M2.** The remaining findings are
documentation / forward-compat / minor gating hygiene.

---

## 2. RANKED FINDINGS

### BLOCKER B1 — `setStartProcInit()` runs fully for puppets (host horse repositioned, force-ride, proc/demo state)

**Claim:** The create path neutralizes story init for puppets by replacing
`setStartProcInit()` with `procWaitInit()`.

**Evidence (src/d/actor/d_a_alink.cpp:5128-5137):**
```cpp
    int midna_prm = setStartProcInit();
#if TARGET_PC
    if (isPuppetCreate) {
        // No story init for puppets: setStartProcInit reads the host save ...
        midna_prm = 0;
        procWaitInit();
    }
#endif
```
`setStartProcInit()` (d_a_alink.cpp:4705) is **called unconditionally**; the
guard only overrides `midna_prm` and calls `procWaitInit()` *afterward*.
`setStartProcInit()`'s body runs in full for the puppet, including:

- `setDamagePoint(getLastSceneDamage(), …)` — neutralized by the .inc guard
  (good), but:
- `getLastSceneMode()`/`getStartMode()` read the **host save / host stage-entry
  param**. For a puppet, `getStartMode()` = `(fopAcM_GetParam(this) >> 0xC) & 0x1F`
  = `0` (puppets are spawned with gparam `0` in PumpSpawns), and
  `getLastSceneMode()` = the host's last-scene mode (global save).
- The proc-init cascade then runs unguarded for the puppet, e.g.:
  - `last_mode == 8` → `initForceRideHorse(); procHorseComebackInit();`
  - `last_mode == 6` → `procCoPeepSubjectivityInit();`
  - `last_mode == 11` → `procBoardWaitInit(fopAcM_SearchByID(mRideActorID));`
  - `last_mode == 12` → `procDungeonWarpSceneStartInit();`
  - `last_mode == 9` → `procWolfDigThroughInit(1);`
  - `last_mode == 4 || 5` → `commonLargeDamageUpInit(-2, TRUE, 0, 0);`
  - the catch-all `start_mode == 0/1/2/3/5/13/14` branch (puppet hits
    `start_mode == 0`) → `mDemo.setStartDemoType();
    mDemo.setDemoMode(DEMO_UNK_14_e); mDemo.setTimer(35);` and, if
    `checkModeFlg(0x400)`, `horse->changeOriginalDemo();
    horse->setSpeedF(…); procHorseWaitInit();` operating on
    `dComIfGp_getHorseActor()` — **the host's horse**.
- `isHorseStart` / `checkCanoeStart()` / `checkBoarStart()` in the create's
  phase-2 wait condition (d_a_alink.cpp:5068-5086) likewise read host save; on
  a horse stage the puppet's create waits on `(isHorseStart &&
  dComIfGp_getHorseActor() == NULL)` (satisfied by the host's horse), then
  proceeds into the setStartProcInit horse branch.

**Why it matters:** On Hyrule Field (or any stage entered with
`last_mode == 1` or `8`), every puppet spawn calls `horsep->setHorsePosAndAngle(
&puppet->current.pos, …)` / `horse->setSpeedF(…)` on the **host's horse**,
teleporting/re-speeding it to the puppet's spawn point (120 units in front of
the real Link), and `initForceRideHorse()` sets the puppet's `mRideActorID` to
the host's horse. `procWaitInit()` afterward resets `mProcID` to `PROC_WAIT`
but does **not** undo the horse reposition, the force-ride state, `mDemo` start
state, or any other proc-init side effects. This is host world-state corruption
on the common case (horse stages) and contradicts both the plan ("setStartProcInit
replaced by procWaitInit") and the commit message.

**Fix:** Skip `setStartProcInit()` entirely for puppets — call `procWaitInit()`
*instead*, before the `int midna_prm = setStartProcInit();` line:
```cpp
#if TARGET_PC
    if (isPuppetCreate) {
        midna_prm = 0;
        procWaitInit();
    } else
#endif
    {
        midna_prm = setStartProcInit();
    }
```
(The horse/canoe/boar wait-conditions in phase 2 should also be suppressed for
puppets — they depend on host save entry mode and can stall or mis-route the
create; at minimum, force `isHorseStart/checkCanoeStart/checkBoarStart` to false
for puppets so the create doesn't wait on / bind to host ride actors.)

---

### MAJOR M1 — Roster not rebroadcast to existing clients on join (3rd+ player invisible)

**Claim:** "PlayerId↔peer mapping correct through the host relay" / 2-8 players.

**Evidence (src/dusk/net/session.cpp:279-336, OnJoinRequest):** when a new peer
joins, the host assigns a PlayerId, updates its own `roster_`, and sends
`JoinAccept` + `WorldInit` **only to the joining peer** (`SendToPeer(peerIndex,
JoinAccept)` / `SendToPeer(peerIndex, WorldInit)`). There is no
`SendToAll(WorldInit, except=newPlayerId)` (or any roster-refresh message) to
existing joined clients. `OnPlayerLeave`/`RemovePlayer` *does* broadcast
`PlayerLeave` to the rest (session.cpp:521-535) — so join and leave are
asymmetric.

**Why it matters:** Client A (joined first) never learns that B joined: A's
`roster_[B].present` stays false. In `dusk::coop::PumpSessionAndSpawns`
(coop.cpp), the spawn gate is `roster[i].present && i != SelfIdChecked()`, so A
never requests a puppet for B, and `RemoteInOurRoom` (sender gate) never sends
A's PlayerState to B (B's slot isn't present in A's roster). Result: later
joiners see earlier players, but earlier players do not see later joiners.
2-player (host + A) works; 3+ is broken. The selftest does not catch this — its
relay test joins A and B then checks message routing, not A's roster post-B-join.

**Fix:** On a successful join, broadcast the updated roster to all already-joined
peers — cheapest is `SendToAll(MsgType::WorldInit, <current full roster>,
exceptPlayer=newId)` right after the new player is added. (Document that
`WorldInit` doubles as a roster-refresh, or add a dedicated `RosterUpdate`.)

---

### MAJOR M2 — Initial equip / item-joints never synced to a freshly-spawned puppet

**Claim:** "remote Link mirrors pose exactly" / appearance matches.

**Evidence:** `PlayerStateMsg` (include/dusk/net/protocol.h) carries `form`,
pose, baseTR, face, stateFlags, roomNo — **not** `mEquipItem`, `mSelectItemId`,
`mLeftItemJntNo`, `mRightItemJntNo`. Those travel only via the reliable
`PlayerEventId::Equip` event, sent **on change** in `SendEventsOnChange`
(coop.cpp). `SendEventsOnChange` is only called from `sendPlayerState`, which
early-returns when `!RemoteInOurRoom(link)`; and the change-trackers
(`g_lastSentEquip`, etc.) are only updated when a send actually fires. A puppet
is created with the **host save's** equip (via `setSelectEquipItem(FALSE)` in
create, which reads `checkWoodSwordEquip`/`checkMasterSwordEquip`/`checkSwordGet`
from the host save). If the remote player's equip differs from the host save at
spawn time, the puppet renders with the wrong sword/shield/held-item model and
wrong item-joint indices until the remote player happens to change equipment.

**Why it matters:** Persistent cosmetic mismatch for the lifetime of the puppet
until an equip change — undermines "mirrors pose exactly" (the item-joint
indices drive `setItemMatrix` attach positions, so even the pose-driven item
models are placed wrong). Form is covered (form is per-frame in PlayerState +
`DriveFormSwap`), but equip is not.

**Fix:** Force-send the current Equip (and AttentionChange, if M1 cares) on the
transition into same-room contact — e.g. when `RemoteInOurRoom` flips
false→true, or when a puppet for us transitions to Active on a remote, reset the
change-trackers to a sentinel so the next `SendEventsOnChange` emits the full
current set. Alternatively, add `equipItem`/`selectItemId`/item-joints to
`PlayerState` (cheap; the packet is already ~2 KB).

---

### MAJOR M3 — Spec drift: 00-network.md §5 PlayerState is the stale joint-list layout

**Claim:** "the new message payloads/fields documented in 00-network.md."

**Evidence (docs/design/mod-coop/00-network.md §5, PlayerState block):** the doc
still describes the provisional **joint-list** layout:
```
playerId u8; scene u8; form u8; movementFlags u8; jointCount u8;
cosmetics u8×3; itemAction s8; invincibility u8; reserved u8;
stateFlags u32; pos f32×3; rot s16×3; upperLimbRot s16×3;
joints Vec3s16[40];  ~= 279 bytes/player/frame
```
The shipped v3 `PlayerStateMsg` (include/dusk/net/protocol.h) is the Rev 3 D4
**raw-matrix** layout: `playerId u8; roomNo s8; form u8; stateFlags u8;
jointCount u8; scaleFlags[5]; yaw s16; pitch s16; faceBckIdx u16; faceBtpIdx u16;
faceFrame s16; reserved u8; pos Vec3f; baseTR Mtx; joints Mtx[40]` (~2 KB).
The doc fields `scene` (vs `roomNo`), `movementFlags`, `cosmetics`, `itemAction`,
`invincibility`, `rot s16×3`, `upperLimbRot s16×3`, and `stateFlags u32` (impl
is `u8`) do not match the implementation. `PlayerEvent` likewise gained a
`data2` (and `scene`/`reserved`) field not documented in §5. §6's bandwidth
"~2.7 KB" assumes a 4×4 Mtx; TP's `Mtx` is `f32[3][4]` = 48 B (libs/dolphin/
include/dolphin/mtx.h:22), so the real wire size is ~2.0 KB.

**Why it matters:** The plan repeatedly flags doc drift as a failure mode; the
normative wire spec is now wrong again. Anyone implementing a client from the
doc would build the wrong packet.

**Fix:** Rewrite 00-network.md §5 PlayerState + PlayerEvent to the v3 raw-matrix
structs (make `protocol.h` the normative reference, as the doc already claims).
Correct the §6 bandwidth figure to ~2.0 KB (48 B Mtx).

---

### MINOR m1 — Stale wire-size comment (2657 vs 2001)

**Evidence (src/dusk/net/protocol.cpp, `WireSize` PlayerState case + the
`PlayerStateWireSize` comment):** comment says `// 2657 — raw-matrix pose (Rev 3
D4)`, but `PlayerStateWireSize()` = `5 + 5 + 10 + 1 + 12 + sizeof(Mtx) +
40*sizeof(Mtx)` = `2001` (sizeof(Mtx)=48). The value returned is correct (2001);
only the comment is wrong (assumes 64-B Mtx). The selftest's
`WireSize(PlayerState) == PlayerStateWireSize()` check is tautological and would
pass either way; the round-trip test proves serializer ↔ WireSize consistency.

**Fix:** Replace `2657` with `2001` in the comment (or compute it from
`sizeof(Mtx)` in the comment).

---

### MINOR m2 — `modelCalc` re-assert runs for the real Link even with net disabled (not byte-for-byte vanilla)

**Evidence (src/d/actor/d_a_alink.cpp:19489-19509):** the mtxCalc re-assert is
gated only on `#if TARGET_PC` and `i_model == mpLinkModel` — **not** on
`dusk::coop::sessionActive()` or `isPuppet(this)`. With `net.enabled=false` and
a single Link, every `modelCalc(mpLinkModel)` now performs 3 extra
`getJointNodePointer(n)->setMtxCalc(field_0x1f20/1f24)` calls per frame before
`calc()`. Vanilla did not. The writes set the same calc pointers
`changeModelDataDirect*` already armed, so this is almost certainly harmless,
but it is a behavior change to single-player outside any `isPuppet`/session
guard.

**Why it matters:** The mandate asks "with net.enabled=false, is the game
byte-for-byte vanilla?" and plan R13 tracks guard hygiene. The other M1 guards
(`execute`, `draw`, `allAnimePlay`, destructor, damage, wolf) are correctly
no-ops when `isPuppet` is false; `modelCalc` is the one always-on deviation.

**Fix:** Gate the re-assert on `dusk::coop::remoteCount() > 0 ||
dusk::coop::isPuppet(this)` (so single-player is untouched), or document it as
intentionally always-on and accept the (tiny) overhead.

---

### MINOR m3 — No `seq` on PlayerState; no staleness/timeout on receive

**Evidence:** `PlayerStateMsg` (protocol.h) has no `seq` field; 02-player-state.md
§2 specifies `seq u16`. `ReceiveSlot::lastFrame` (coop.cpp) is updated on receive
but never read — there is no seq-gap detection, no freeze/fade timeout. A remote
that stops sending (crashed without leaving the session) leaves its puppet held
in the last applied pose forever. This is functionally "freeze last pose" (the
stale-packet policy), so behavior is acceptable for v1, but the explicit
"seq-gap timeout → freeze" mechanism from the plan is not implemented. Matches
00-network.md (no seq) but drifts from 02-player-state.md §2.

**Fix:** Either add `seq u16` and implement a timeout, or reconcile 02's §2 with
00's §5 (drop `seq` from 02) and document "stale = last pose held, no fade in
M1."

---

### MINOR m4 — Host `worldStage_` never filled from the sim; `stageOk` check is dead

**Evidence (coop.cpp, `EnsureSession`):** `cfg.stage.stage[0] = '\0'` for both
host and client; the host never updates `worldStage_` from the real Link's
stage. In `ApplyPuppetState`, `stageOk = strcmp(getStartStageName(),
g_session.worldStage().stage) == 0 || g_session.worldStage().stage[0] == '\0'`
is always true (empty stage short-circuits). So the hidden-state check reduces
to `st.roomNo != LocalRoomNo()` only. The plan says "M1+ fills it from the real
sim (stage/room/spawn)" — not done. Acceptable for M1 (join-warp is M4, LAN
same-stage testing), but the `stageOk` guard is currently a no-op and should be
either implemented or marked `// TODO M4` explicitly.

**Fix:** Fill `worldStage_` from the real Link's stage on the host (and broadcast
it on stage change) in M4; until then, comment the `stageOk` line as intentionally
inert in M1.

---

### MINOR m5 — Sender gate sends to peers whose room is unknown

**Evidence (coop.cpp, `RemoteInOurRoom`):** `if (!slot.hasState ||
slot.state.roomNo == myRoom) return true;` — a present remote from whom we have
not yet received state is assumed same-room and sent to. On a star where the
remote is gating their own sends the same way, two players in different rooms
both send to each other (wasted ~2 KB/frame each way) and both keep their
puppets hidden via roomNo mismatch.

**Fix:** Tighten to `slot.hasState && slot.state.roomNo == myRoom` (Anchor's
"only same-scene peers receive your updates"). The cost is a 1-frame delay on
first same-room contact; acceptable.

---

### MINOR m6 — The aurora "side quest" commit was reverted at HEAD; M1 commit list is stale

**Evidence:** The mandate lists M1 commits ending at `7845c1282c` ("fix: game
boot with aurora 8-controller pad; pin aurora submodule"). `git log --oneline`
shows HEAD is actually `6551bb53f1` ("revert: aurora to upstream pointer
(6c4c27f9); drop 8-controller boot guard"), dated ~2h after the M1 commits,
which reverts `7845c1282c`: `extern/aurora` is back to the upstream 4-controller
pointer (`6c4c27f`, `PAD_CHANMAX 4`), and the `action_bindings.cpp` guard is
dropped. The build at HEAD succeeds with the upstream aurora.

Assessment of the side quest itself (`7845c1282c`, now reverted): it was a
legitimate boot fix for the newer 8-controller aurora (`configVars->at(port)`
for ports 4-7 threw `std::out_of_range`), but it was **out of M1 scope** (a
build/boot fix unrelated to player replication) and the submodule pin
(`abb0c3d`) was a dirty pointer not recorded as a known-good upstream commit —
the revert restores upstream tracking. **The revert is sound and is the
correct state to ship.** Risk at HEAD: none (build passes, net code does not
touch the pad system). The only issue is that the mandate's M1 commit list
omits `6551bb53f1`, so a reviewer checking only the listed commits would not
see that the pin was undone.

**Fix:** None at HEAD. Note in the M1 record that `7845c1282c` was reverted by
`6551bb53f1`; the M1 series as shipped is the 4 net/coop/alink commits
(`f5710cb60f` → `195093aaae`) plus the aurora revert.

---

### MINOR m7 — Forward-compat: host relay conflates "game message" with "relay to all"

**Evidence (src/dusk/net/session.cpp:249-257, 436-450):** `HandleData` calls
`gameHandler_` then `ForwardGameMessage(originPeer, …)` for **both**
`PlayerState` and `PlayerEvent`. `ForwardGameMessage` relays to every joined
peer except the origin. This is correct for player state/events (star relay).
But the same dispatch shape will be reused for M2's `EnemySnapshot` /
`CombatIntent` / `CombatResult`: a `CombatIntent` (client → host/sim-owner) must
**not** be relayed to other clients (the host validates and emits a
`CombatResult`); `EnemySnapshot` should flow host→clients only (not client→host
then relayed). The current "game message = relay to all" structure will need a
per-type relay policy. The registry (per-stage, PlayerId-keyed) does not box in
enemy authority — good — but the routing layer needs the policy split.

**Fix (M2):** Add a `RelayPolicy(type)` switch in `HandleData`/`ForwardGameMessage`
(host-only-for-enemies, no-relay-for-intents, relay-for-results) rather than
extending the `case PlayerState: case PlayerEvent: relay` fallthrough. Also,
host→all `SendGameMessage` broadcast does not respect room ownership (v2) —
leave as v1 (host owns its room) and revisit for M5.

---

### MINOR m8 — `modelCalc` re-assert covers only `mpLinkModel` (face/hat/item not re-armed)

**Evidence (d_a_alink.cpp:19489-19509):** the re-assert installs
`field_0x1f20/1f24` on joints 0/1/16 (human) / 0/3/15 (wolf) of
`mpLinkModel->getModelData()` only. `mpLinkFaceModel`, `mpLinkHatModel`, and the
sword/shield/held-item models are loaded from the same shared (refcounted) arc
and may share `J3DModelData` across Link instances; their `mtxCalc` pointers are
not re-asserted. The plan (01-puppet-link §6.3 / risk 2) only mandated the body
model, and the face/hat calcs consume the body pose we just wrote (so the body
hazard is the critical one), but a residual face/hat drift between two Links is
possible. Cosmetic; verify at playtest.

**Fix:** If face/hat drift is visible at 2+ players, extend the re-assert to
those models' joint 0 (or whatever index `changeModelDataDirect` arms for them).

---

## 3. VERIFIED-OK (claims checked and confirmed)

- **Build & selftest** — game builds clean; `dusk_net_selftest` PASS (v3
  round-trip + host relay + version-mismatch/session-full rejection + slot
  reuse + SessionEnd propagation).
- **Slot-0 pointer clobber (risk 1)** — save/restore in `create()` is airtight
  within phase 1: `dComIfGp_setPlayer(0,this); setLinkPlayer(this);` is
  immediately followed by the restore for puppets; no intervening early return
  (d_a_alink.cpp:4958-4968). Subsequent phase calls have `bgWaitFlg=TRUE` and
  skip the block. Destructor slot-0 clears (`setPlayer(0,NULL)`,
  `setLinkPlayer(NULL)`, `clearPlayerStatus0/1`) are guarded
  `if (!isPuppet(this))` (d_a_alink.cpp:20024, 20068). `onLinkCreated` correctly
  separates real Link vs puppet.
- **Host save writes (risk 3)** — `setRestartRoom`, all four
  `setSelectEquipClothes` sites, `setItem(SLOT_18, TKS_LETTER)`, Midna/NPC_TK,
  CANOE/IceLeaf, portal `setPtD`, `setItemActor`, light-ball `setForceGrab`,
  portal-warp-miss tag, LV2 switch writes — all guarded `if (!isPuppetCreate)`.
  `setDamagePoint`/`setDamagePointNormal`/`setLandDamagePoint` (damage.inc) and
  `changeWolf`/`changeLink` `setTransformStatus` (wolf.inc) skip for puppets.
  (Caveat: `setStartProcInit` still runs — see B1; the `setDamagePoint` *call
  inside it* is neutralized by the .inc guard, but the horse/proc side effects
  are not.)
- **Shared J3DModelData mtxCalc (risk 2, body model)** — `modelCalc` re-asserts
  `field_0x1f20/1f24` on joints 0/1/16 (human) / 0/3/15 (wolf) before `calc()`,
  matching the fork's indices (01-puppet-link §6.3).
- **Apply pipeline order** — `ApplyPuppetState` matches plan Rev 3 §5 M1 exactly:
  transform → form (`DriveFormSwap`) → `mProcID=PROC_WAIT` → face bck/btp →
  `setMatrix` → `allAnimePlay` → face frame + `playFaceTextureAnime` →
  `modelCalc` → per-joint `setAnmMtx` + `setScaleFlag` + `setBaseTRMtx` →
  `setItemMatrix`/`setWolfItemMatrix` → `setBodyPartPos` → `setAttentionPos` →
  `setCollisionPos`/`setWolfCollisionPos` → per-frame Tg registration (3 cyls) →
  `mLinkAcch.CrrPos` → `setRoomInfo`. No missing/reordered step.
- **Collider registration (risk 6)** — three `mTgCyls` re-registered via
  `dComIfG_Ccsp()->Set` + `SetMass(..,1)` every apply (cCcS clears per frame).
- **Form atomicity (risk 11)** — `DriveFormSwap` holds the last pose + marks the
  puppet hidden while the target arc loads (`resDelete` + `cPhs_Reset` +
  `freeAll` + `setArcName` + `resLoad`), then `changeWolf`/`changeLink`, re-arms
  ANM_WAIT packs, and falls through to pose the new skeleton the same frame the
  load completes.
- **Frame-interp (risk 12)** — pose copy uses the interp-aware
  `J3DModel::setAnmMtx(j, mtx)` / `setBaseTRMtx` only; no raw `mMtxBuffer` writes.
- **Sender frame-final read (§4)** — `sendPlayerState` is the tail of the real
  Link's `execute()`, after `setMatrix`/`modelCalc`/`setItemMatrix`/
  `setBodyPartPos` (d_a_alink.cpp:19044). Puppets return early from `execute`
  before the sender hook, so only the real Link sends.
- **60 Hz cadence (D2)** — sender runs every `execute`; no timer/batching.
- **jointCount safety (R11)** — `DeserializePlayerState` rejects
  `jointCount > kMaxJoints`; `ApplyPuppetState` copies
  `min(st.jointCount, localJoints)`; send diagnostic baseTR index fix
  (`195093aaae`) correct (`baseTR[2][3]`, Mtx is 3×4).
- **Frozen-in-cutscene** — `stateFlags & kPlayerStateFlagDemo` early-returns
  from `ApplyPuppetState` (holds last pose); sender sets the demo bit from
  `mDemo.getDemoType() != 0`.
- **Hidden-state** — room mismatch → `entry.hidden=true` → `ApplyPuppetState`
  early-returns (no pose work) and `draw()` early-returns
  (`puppetDrawHidden`); fresh puppets start hidden until first state.
- **Stage-change survival** — puppets spawn on the real Link's stage layer
  (PumpSpawns sets `fpcLy_SetCurrentLayer(realProc->layer_tag.layer)`), so they
  survive room sweeps; on stage death, `~daAlink_c` → `onLinkDestroyed` clears
  the entry and `PumpSpawns` re-requests for still-present players (polling
  fallback, per 01-puppet-link §6.5). Serialized creates (`g_createInFlight`)
  protect the shared static `bgWaitFlg` (R4).
- **Star-topology relay (§2)** — host relays inbound PlayerState/PlayerEvent to
  all joined peers except origin; `SendGameMessage` is host→all (SendToAll) /
  client→host (SendToPeer(0)); origin never receives its own message
  (selftest-verified).
- **Duplicate-join guard** — `OnJoinRequest` rejects a second JoinRequest from a
  peer that already holds a PlayerId (session.cpp:279-284); carried through into
  game glue (roster is the single source of truth for spawn decisions).
- **Gating / vanilla path** — with `net.enabled=false`, `EnsureSession` returns
  early, `SessionLive()` is false → no spawns/sends; `isPuppet` always false →
  `execute`/`draw`/`allAnimePlay`/damage/wolf/destructor run vanilla. The only
  always-on deviation is `modelCalc` (m2). All guards are `#if TARGET_PC` with
  original code intact and upstream-PR-able.
- **Graceful shutdown** — `dusk::coop::shutdown` (wired into
  `dusk::config::shutdown`, config.cpp:649) calls `Session::Stop` which sends
  PlayerLeave (client) / SessionEnd (host) then stops/joins transport.

## 4. TOP MUST-FIX before M2

1. **B1** — Skip `setStartProcInit()` for puppets (call `procWaitInit()`
   *instead*, not in addition); also suppress the horse/canoe/boar
   wait-conditions in create phase 2 for puppets so the create doesn't bind to
   host ride actors. **This is the one real correctness regression vs. the plan.**
2. **M1** — Broadcast the roster to existing clients on join (re-send `WorldInit`
   to all on success) so 3+ players see each other.
3. **M2** — Force-send Equip (and item joints) on first same-room contact /
   puppet activation, or add equip to `PlayerState`, so freshly-spawned puppets
   don't render with host-save equipment.
4. **M3** — Update 00-network.md §5 PlayerState + PlayerEvent to the v3
   raw-matrix structs; correct the §6 bandwidth figure (48-B Mtx → ~2.0 KB).

## 5. Spec drift vs 00-network.md (summary + recommendations)

| Area | Doc | Impl (v3) | Recommendation |
|------|-----|-----------|----------------|
| PlayerState payload | §5 joint-list (Vec3s16[40], cosmetics, itemAction, invincibility, rot, upperLimbRot, stateFlags u32, ~279 B) | raw Mtx[40] + baseTR + scaleFlags + face + stateFlags u8, ~2.0 KB | Rewrite §5 to the `PlayerStateMsg` struct; make `protocol.h` normative |
| PlayerEvent payload | §5 "player id + event + data" | + `scene` u8, `reserved` u8, `data2` u32 | Document `data2` (item-joint payload) and `scene` |
| Bandwidth | §6 "~2.7 KB" (4×4 Mtx) | ~2.0 KB (3×4 Mtx = 48 B) | Correct to ~2.0 KB |
| `seq` field | 02-player-state §2 has `seq u16`; 00 §5 has none | none | Reconcile 02 ↔ 00 (drop seq from 02, or add it + implement timeout) |
| WorldInit on join | §4 "sent right after JoinAccept" | sent to joining peer only (no rebroadcast) | Document WorldInit as also a roster-refresh broadcast on join (see M1) |
| Host world stage | §4/plan "M1+ fills real values" | empty (`stage[0]='\0'`); `stageOk` dead | Mark as M4 TODO or implement |

No drift detected in: message envelope (`u16 type + u16 size + LE payload`),
channel split (reliable vs unreliable-seq), star relay topology, JoinRequest
version gate, JoinAccept/WorldInit fixed-size roster, or the `kMaxJoints=40` /
`kMaxLocalPlayers=8` constants.
