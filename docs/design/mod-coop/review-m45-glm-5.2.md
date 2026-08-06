# M4.5 adversarial review — final fix pass (stay-put join + EnemyEvent scoping)

Review range: `0fa641b36b..HEAD` (M4.5 commits `ea68f73c96` coop: remove
join-warp, `9904fb8403` net: EnemyEvent room-scoped routing, `8fd89c2bf7`
net: SceneChange stage key, `4ce3365b68` coop: drop dead code,
`e942738d79` docs: discovery log + join-warp removal, `99d50fd95d` M4.5
complete), branch `net-coop`. Aurora pinned at upstream `6c4c27f9`
(unchanged — not a regression source). Baseline: `review-m4-glm-5.2.md`
(2 MAJORs + MINORs 1-9).

## Evidence gathered (all real)

- **Forced clean rebuild of the selftest**: `ninja -t clean
  dusk_net_selftest` (20 files) → `ninja dusk_net_selftest` → clean
  compile+link (1.98 s, 21 steps, zero warnings).
- **Selftest run**: `./dusk_net_selftest` → **`PASS: all checks
  succeeded`**, **318 `ok` checks** (up from M4's 306 — the M4.5 suites added
  the cross-stage SceneChange block, the room-scoped EnemyEvent assertions,
  and the renamed worldStage-carry suite). M4 suites still green:
  `m4: room ownership table`, `m4: room-owner routing + same-room
  snapshot/event scoping` (now with EnemyEvent rows), `m4: worldStage carry
  (stay-put join policy, D6 revised)`, `m4: entity-id stability`, `m4: LAN
  discovery HostAnnounce`. The `wire: deterministic serialization` sweep now
  runs through `MsgType::RoomOwnership` (16).
- **Forced clean rebuild of the game**: `ninja -t clean dusklight` (2248
  files) → `ninja dusklight` → clean compile+link of the full game binary
  (`Dusklight.app/Contents/MacOS/Dusklight`, 48.6 MB) + `dusklight-stub`.
  One initial pass returned exit 1 mid-way (a transient subcommand failure
  during the 2248-file rebuild); resuming `ninja dusklight` completed with
  exit 0 and `ninja: no work to do` on re-check — the binary is freshly
  linked. Re-touching every M4.5 TU (`coop.cpp`, `coop_enemy.cpp`,
  `session.cpp`, `discovery.cpp`, `protocol.cpp`, `selftest_main.cpp`) and
  rebuilding produced **zero warnings from the touched TUs**; the only
  warning emitted in the incremental rebuild is the pre-existing vanilla
  `JKRExpHeap.h:30` multichar constant (a JSystem header pulled in
  transitively, untouched by M4.5). No M4.5 TU warns.
- **Source cross-checks** (grep): the join-warp machinery
  (`DecideJoinWarp`/`PerformJoinWarp`/`DriveJoinWarp`, `g_reachedStages`,
  `g_warpPending`, `g_warpRefusedNotified`, `g_saveStageSeeded`,
  `sSaveStageSeeded`, `SafeStageSet`, `JoinWarpDecision`,
  `coop_join_logic.h`, `dMeter2Info_getSaveStageName`) is **fully absent
  from `src/dusk/` and `include/dusk/`** — only `docs/` references remain
  (the M4 review itself + the implementation-plan's "removed" record). The
  dead-code quartet (`coop::setWorldStage`, `sessionEndReason`,
  `hostWorldStageKnown`, `g_lastDiscoveryLogMs`) is gone from source;
  `Session::setWorldStage` (inline in `session.h:187`) is correctly RETAINED
  — the host fills `worldStage_` every frame. No `dStage_changeScene` or
  `dComIfGp_setNextStage` call survives in `src/dusk/coop/`.
- **Live-game note**: the TP RVZ used for the M4 loopback runs was removed
  from the repo root (gitignored by `c4ca64a356`), so a fresh full-stack
  playtest could not be re-run in this environment. The stay-put join path
  is therefore exercised by the selftest (worldStage carry + the stageOk
  gate's wire-stage comparison) and by source inspection, not by a live
  two-instance run. The M4 live-loopback evidence (cited in the M4 review)
  predates M4.5 and is not re-verified here.

---

## VERDICT

**M4.5 resolves both MAJORs and the targeted MINORs. HEAD is buildable,
selftest-green (318/318), and regression-clean. The branch is ready for the
final full-stack playtest** (pending a live two-instance run, which this
environment cannot host without the RVZ).

- **MAJOR 1 (join-warp) — RESOLVED by removal (user decision).** The entire
  warp machinery is deleted; a joining client boots into its own save stage
  and stays. The `worldStage_` fill + JoinAccept/WorldInit carry are kept
  and selftested; the cross-stage `stageOk` puppet gate (keys on the
  remote's REAL stage from `PlayerState`, not the worldStage heuristic)
  hides the host's puppet from a cross-stage client — the stay-put policy's
  visibility contract holds by construction.
- **MAJOR 2 (unscoped EnemyEvent) — RESOLVED.** `EnemyEvent` is now
  `RelayPolicy::RoomScoped` (shared with `EnemySnapshot`), and a
  receive-side local-room gate drops any leaked non-room event. The relay
  keys on `(stage, room)` via `SendToAllInRoom`, so a cross-stage peer with a
  coincident `(roomNo<<8)|setID` never receives the event; the receive-side
  gate is room-only defense-in-depth. Selftest asserts a room-2 died event
  is not delivered to a room-1 peer and not echoed to the owner.
- **MINOR 1 (SceneChange stage key) — RESOLVED.** `PlayerEventMsg.scene`
  carries a same-stage flag (1 = within-stage, 0 = stage changed); the host
  keys the room table only on `scene==1`. Selftest 0b proves a cross-stage
  SceneChange creates no bogus `(oldStage, newRoom)` owner entry.
- **MINOR 2 (dead code) — RESOLVED.** All four dead items removed; the
  retained `Session::setWorldStage` is the live one.
- **MINOR 4 (doc drift) — RESOLVED.** `00-network.md` §10 + §4 row now say
  `discovery: found session ...` (matches `discovery.cpp:312`).
- **MINOR 5 (determinism sweep) — RESOLVED.** Loop bound is
  `MsgType::RoomOwnership` (16); all 16 types swept.
- **MINOR 6 (0-player announces) — RESOLVED.** `SetPlayers` is called
  before `Announcer::Start` in `DriveDiscovery`; the selftest discovery
  suite also seeds before `Start` and observes `players 2/4`.
- **MINOR 8 (sSaveStageSeeded static) — RESOLVED by removal** of the warp
  feature (the static went with it).
- **Regression — CLEAN.** Forced clean game rebuild links; touched TUs
  compile warning-free; `net.enabled=false` still gates the session
  (`EnsureSession` returns early); no M5 features (Spawned still
  logged-not-created, no horse channel, no PvP); M1/M2/M3 core logic
  untouched (the only M2 edit is the additive receive-side room gate; the
  only M1 edit is the `scene` flag in `sendPlayerState`).

## RANKED FINDINGS

### BLOCKER
None. (No build/link/selftest failure; nothing prevents booting the game
or running the co-located playtest.)

### MAJOR
None. Both M4 MAJORs are resolved and no new MAJOR was introduced.

### MINOR

**MINOR A — The receive-side EnemyEvent room gate is room-only; the
cross-stage hazard is closed by the relay, not by the receive gate.**
`coop_enemy.cpp:1061-1066` (Died) and `:1071-1076` (RoomClear) compare
`ev.enemyId >> 8` (Died) / `ev.enemyId` (RoomClear) against
`coop::localRoomNo()` — a room number, not a stage. The original MAJOR 2
concern was a **cross-stage** coincident id; the receive gate alone would
not catch a same-room-different-stage leak (it only checks the room).
The protection is in fact the relay: `SendToAllInRoom`
(`session.cpp:985-988`) keys on **both** `room.room` AND `room.stage`, so a
cross-stage peer never receives the event. This is sound and matches the
documented design ("the relay is room-scoped now; the receive side is
defense in depth" — `coop_enemy.cpp:1054`). The only residual risk is
 brittleness: if a future change made the relay room-only (dropping the
 stage compare), the receive gate would not compensate. **No fix required
 now**; worth a one-line comment on the receive gate noting it relies on
 the relay's stage-scoping for the cross-stage case.

**MINOR B — The selftest does not directly assert the cross-stage
EnemyEvent case (the original MAJOR 2 hazard).** `RunM4RoomRoutingCheck`
step 2b (`selftest_main.cpp:2203-2216`) asserts a room-2 died event is
not delivered to peer A while A is in room 1 — that is the **same-stage
different-room** case. The cross-stage coincident-id case (the actual
MAJOR 2: stage A room R setID S vs stage B room R setID S) is covered
indirectly because `EnemyEvent` and `EnemySnapshot` now share
`SendToAllInRoom` (whose stage-scoping is exercised by the M4 snapshot
tests), but no test sends an EnemyEvent from a sender in stage X and
asserts a receiver in a different stage with the same room number does
not get it. The shared relay path makes a regression implausible, but an
explicit cross-stage EnemyEvent assertion would lock the MAJOR 2 fix
exactly. **No fix required**; optional hardening.

**MINOR C — `dusklight` clean-rebuild transient.** The first
`ninja -t clean dusklight && ninja dusklight` pass returned exit 1
("subcommand failed") during the 2248-file rebuild; resuming
`ninja dusklight` completed cleanly (exit 0, binary freshly linked, no
errors in the resume log). No source error was captured — the failure
was a transient mid-rebuild subcommand (likely a compiler/resource hiccup
under the parallel 2248-file load), not an M4.5 defect: a re-touch of
all M4.5 TUs + rebuild is warning-free and the selftest is green. Noting
for completeness; not actionable.

## VERIFIED-OK

1. **Join-warp fully removed (MAJOR 1).** `coop_join_logic.h` is deleted
   from the tree (`git show HEAD:include/dusk/coop/coop_join_logic.h` →
   "does not exist"); grep for `DecideJoinWarp`/`PerformJoinWarp`/
   `DriveJoinWarp`/`SafeStageSet`/`JoinWarpDecision`/`g_reachedStages`/
   `g_warpPending`/`g_saveStageSeeded`/`sSaveStageSeeded` across
   `src/dusk/`+`include/dusk/` returns nothing. The `d/d_meter2_info.h`
   include is dropped from `coop.cpp`. `shutdown()` no longer resets the
   warp state. No `dStage_changeScene` / `dComIfGp_setNextStage` /
   `setRestartRoom` call remains in `src/dusk/coop/` — the coop layer
   never forces a teleport.
2. **Stay-put join path retained correctly.** `worldStage_` is filled
   from the real Link every frame (`coop.cpp:1185-1199`,
   `g_session.setWorldStage(st)`) and carried in JoinAccept
   (`session.cpp:653`) + WorldInit (`session.cpp:664`); the selftest
   `m4: worldStage carry` asserts the client receives the host's filled
   `(stage, room)`. The cross-stage `stageOk` gate (`coop.cpp:526-529`)
   keys on the wire `st.stage` (`sameStage = strcmp(st.stage,
   LocalStageName())`) — NOT the worldStage heuristic — so a client whose
   stage differs from the host's hides the host's puppet
   (`entry.hidden = !sameStage || st.roomNo != LocalRoomNo()`). The
   stay-put visibility contract holds.
3. **EnemyEvent room-scoped (MAJOR 2).** `PolicyFor(EnemyEvent) =
   RelayPolicy::RoomScoped` (`session.cpp:64-74`, grouped with
   `EnemySnapshot`). Host fan-out (`SendGameMessage`) uses
   `SendToAllInRoom(playerRoom_[0])`; client-owner relay
   (`ForwardGameMessage`) uses `SendToAllInRoom(playerRoom_[originPid])`;
   `SendToAllInRoom` (`session.cpp:985-992`) skips any peer whose
   `(stage, room)` differs from the sender's. Receive-side defense:
   `coop_enemy.cpp:1061-1066` drops Died events whose decoded room
   (`enemyId >> 8`) ≠ `localRoomNo()` (stage-placed ids only; dynamic ids
   ≥ `kDynamicIdBase = 0x8000` are owner-scoped and skip the gate —
   correct, dynamic ids are owner-major and cannot cross-stage collide);
   `:1071-1076` drops RoomClear whose bare room ≠ `localRoomNo()`. The
   relay fires independent of the game handler (`session.cpp:545/559`
   calls `gameHandler_` then `ForwardGameMessage`), so a host not in the
   event's room correctly no-ops the event locally while still relaying
   it to the room's peers.
4. **SceneChange stage key (MINOR 1).** `sendPlayerState`
   (`coop.cpp:1085-1091`) computes `stageChanged = strcmp(myStage,
   g_lastSentStage) != 0` and passes `scene = stageChanged ? 0 : 1` via
   `SendPlayerEvent(..., scene)`. The host handler (`session.cpp:518-540`)
   keys the room table only when `ev.scene == 1` AND
   `playerRoom_[ev.playerId].stage[0] != '\0'`. Selftest 0b
   (`selftest_main.cpp:2099-2157`) asserts: a cross-stage SceneChange
   (scene=0) does not move the table (`host.playerRoom(a).room == 2`
   holds), no bogus `(F_SP108, 7)` owner is created
   (`host.roomOwner("F_SP108", 7) == kInvalidPlayerId`), and the first
   new-stage PlayerState establishes `(F_SP104, 7)` with A as owner. The
   same-stage return move (scene=1) keys correctly.
5. **Dead code dropped (MINOR 2).** `coop::setWorldStage`
   (`coop.h`+`coop.cpp`) removed; `coop::sessionEndReason` removed;
   `coop::hostWorldStageKnown` removed; `g_lastDiscoveryLogMs` (decl +
   reset) removed. `Session::setWorldStage` (inline, `session.h:187`)
   is the retained live accessor. No new dead code introduced.
6. **Doc + determinism + announce fixes (MINORs 4/5/6).** `00-network.md`
   §10 + §4 HostAnnounce row say `discovery: found session ...`
   (matches `discovery.cpp:312`); §2 records the EnemyEvent room-scoping
   fix; §4 "Join mid-game" documents the stay-put policy; §11 marks the
   forced-stage-change + spawn-anchor open items RESOLVED. The
   determinism sweep bound is `MsgType::RoomOwnership` (16)
   (`selftest_main.cpp:446`) — all 16 types covered. `DriveDiscovery`
   (`coop.cpp:918`) calls `SetPlayers(remoteCount()+1)` BEFORE
   `Announcer::Start`; the selftest discovery suite does the same and
   observes `players 2/4` on the received datagrams.
7. **Selftest evidence for the room-scoping fix.** `RunM4RoomRoutingCheck`
   2b: B (room-2 owner) sends `EnemyEvent(died)` for `0x0203` with
   `flagMask=0x04`; A is in room 1 → `aEvents == 0` (not relayed to the
   room-1 peer), `bEvents == 0` (not echoed to the owner), `hostEvents >=
   1` (host consumed). After A returns to room 2, B's next died event
   (`0x0204`) reaches A (`aEvents >= 1`). `RunM2RelayPolicyCheck` step 5
   documents the no-room-established open-gate case (star fan-out when no
   room is known — same rule as the sender gate).
8. **Regression — prior milestones intact.** M1 player replication: only
   the `scene` flag added to `sendPlayerState` (additive). M2 enemy
   authority: only the additive receive-side room gate in
   `coop_enemy.cpp`; the owner-authority / snapshot / combat-routing paths
   are unchanged. M3 time-weather: no TU touched. M4 room ownership /
   discovery / host-leave UX: the table, the routing, the discovery
   lifecycle, and the `NoticeSessionEnd` path are unchanged (only
   `DriveJoinWarp` was deleted from the per-frame driver list). Net-off
   boot: `EnsureSession` (`coop.cpp:820-828`) still returns early on
   `net.enabled=false`; all attachment points still check
   `sessionActive`/`isPuppet`. No M5 features: `EnemyEventId::Spawned`
   is still "logged rather than created" (`coop_enemy.cpp:1078`), no
   horse channel, no PvP damage table.
9. **Guard hygiene.** All M4.5 changes are inside `#if TARGET_PC` coop/net
   code; no vanilla source touched. The non-PC path is unchanged.

## MUST-FIX (for the final full-stack playtest)

**None.** Both MAJORs are resolved, the targeted MINORs are resolved, the
build is green, the selftest is 318/318, and no regression is observed.
The only remaining verification is a live two-instance loopback playtest
(exercising: client joins → stays in its own save stage → host's puppet is
hidden → players travel to a shared stage → puppets appear → cross-room
enemy events do not mis-kill), which this environment cannot run without
the gitignored RVZ. The selftest + source inspection cover the stay-put
join path and the EnemyEvent scoping to the extent possible offline.
