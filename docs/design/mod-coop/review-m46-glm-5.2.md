# M4.6 Capstone Fix-Pass — Adversarial Review (glm-5.2)

Review range: `cfdf2cc598..HEAD` on branch `net-coop` (the M4.6 capstone pass:
`a3a3d56fff`..`aca524df39`, 9 commits). This is the **fix pass** against the two
capstone reviews (`review-full-deepseek-v4-flash-0731.md` MAJOR 1 + MINORs
A,C,D,F,G,H,I,J,K,L; `review-full-glm-5.2.md` MAJOR 1, MAJOR 2 + MINORs 1-8).
Aurora is at upstream `6c4c27f9` (not a regression — pre-existing).

Method: read both capstone reviews, the full M4.6 diff, and every touched
source file (session.cpp, transport.cpp/h, coop.cpp, coop_enemy.cpp/h,
coop.h, protocol.h, protocol.cpp, discovery.cpp, selftest_main.cpp) end to
end; then forced clean rebuilds of the selftest + the full game, and the
selftest run twice.

## Evidence gathered (all real, this environment)

- **Forced clean rebuild of the selftest**: `ninja -t clean dusk_net_selftest` →
  `ninja dusk_net_selftest` → clean compile + link, **zero warnings** from any
  net TU. `./dusk_net_selftest` → **`PASS: all checks succeeded`, 361 `ok`
  checks**, exit 0, **run twice** (stable — the MAJOR-1 restart leg B depends
  on the ENet peer timeout and survived both runs). The "reliable outbox ring
  full" log lines are the deliberate flood-overflow test, not a failure.
  (Was 318 at the capstone; +43 from the M4.6 rows + the `unique_ptr<Session>`
  refactor that made the same-process restart exercisable.)
- **Forced rebuild of the touched TUs + full `dusklight` link**: touched every
  net/coop TU + header (`session.cpp`, `coop_enemy.cpp`, `coop.cpp`,
  `discovery.cpp`, `protocol.cpp`, `transport.cpp`, `protocol.h`, `session.h`,
  `transport.h`, `coop.h`, `coop_enemy.h`) → `ninja dusklight` → clean
  compile + link, **33903 exports / 2053 objects**, exit 0. The only warnings
  are the **pre-existing vanilla** ones (`d_a_alink.cpp` → `d_a_alink_demo.inc` →
  JStudio `stb.h` deprecated `std::iterator` + `daObjTks_c::setExpression`
  undefined-inline) and the pre-existing `ld: ignoring duplicate libraries`
  note. **Zero warnings from any net/coop TU.**
- **No vanilla file touched**: `git diff --stat cfdf2cc598..HEAD` shows only
  `src/dusk/net/*`, `src/dusk/coop/*`, `include/dusk/{net,coop}/*`, and
  `docs/design/mod-coop/*`. No `d/`, `f_`, or `libs/` file changed — the net-off
  boot path is byte-for-byte untouched by construction (every M4.6 code path is
  `g_sessionStarted` / `SessionLive()` / `state()`-gated; with `net.enabled`
  off, none fire).
- **Live loopback not re-runnable here**: the TP RVZ is gitignored (`c4ca64a356`),
  so the final two-instance live playtest is the user's. The MAJOR-1 restart
  is exercised by the selftest (both legs), not a live run.

---

## OVERALL VERDICT

**M4.6 resolves all 3 MAJORs and the full MINOR cluster. HEAD is buildable,
selftest-green (361/361, twice), regression-clean (no vanilla file touched; full
`dusklight` link clean; net-off untouched), and READY for the final live
playtest.**

- **MAJOR 1 (transport teardown)** — FIXED + selftested (both the graceful
  `SessionEnd` and the hard `HandleDisconnect` paths; same-process restart
  verified).
- **MAJOR 2 (PollHostDeaths UAF)** — FIXED. `e.adapter` captured at
  `RegisterActor`; `PollHostDeaths` never re-derives it from a freed `e.actor`;
  the only post-registration `e.actor` derefs are the `isDead`/`deathSwitchNo`
  reads, reached only when `!gone`.
- **MAJOR 3 (freeze-latency window)** — FIXED. `ScanAndRegister()` runs
  immediately on room change; an optimistic freeze holds whitelisted, non-owned,
  unregistered enemies on the first frame; the owner's own room still sims
  natively; the 15-frame backstop stays for mid-room spawns.
- **MINOR cluster (A,C,D,F,G,H,I,J,K,L + glm 1-8)** — all addressed
  (SceneChange receive gate, spawn-limiter anchor reset, sender-gate
  consolidation, `onRoomUnload` deletion, per-stage `(stage,room)` reset,
  join/start-failure feedback, `std::map` node-stability hardening + same-frame
  contract comment, `g_roomClear < 128`, `sendPlayerState` isPuppet guard,
  `OnWorldInit` comment, version-history note, dead-wire doc comments,
  2017-B/44-B doc corrections, supersede banners, 04 byte count, RoomClear +
  cross-stage selftest rows).

**BLOCKER: none. MAJOR: none. MUST-FIX: none.** A handful of benign MINOR
observations are recorded below (none blocking; most are the explicit
capstone recommendations, restated here for the M5 developer).

---

## RANKED FINDINGS

### BLOCKER — none.

### MAJOR — none.

### MINOR 1 (observation — benign, no fix required) — `g_entries` is now `std::map`, iteration order changed

**File**: `src/dusk/coop/coop_enemy.cpp:337` (`std::map<u16, EnemyEntry> g_entries;`,
was `std::unordered_map`).

The MINOR-H hardening switched the registry to a node-based container so
`&e.synthAt` / `&e.synthStts` stay stable across inserts (the
`SetTgHitSynthetic` address-stability contract). A side effect is that
iteration is now **sorted by `enemyId` key** instead of hash order. Every
iteration site is order-independent (`PollHostDeaths` collects into a `dead`
vector then resolves; the non-owner Dying sweep erases in place;
`ScanAndRegister`/`ClearAll`/`FindEntryForActor` are key-based). No correctness
dependence on order — but it is a (benign) behavior change worth knowing for
M5 (a wave that expects insertion-order iteration would see sorted order).

### MINOR 2 (observation — benign, no fix required) — the consolidated `remoteInRoom` can over-open vs the old snapshot gate

**File**: `src/dusk/coop/coop.cpp:1449-1457` (`remoteInRoom` now delegates to
`RemoteInOurRoom(LocalStageName(), roomNo)`, `:418-441`).

The old exported `remoteInRoom` (the enemy-snapshot sender gate) checked only
"same-stage + same-room, or unknown-opens". The consolidated
`RemoteInOurRoom` **also** opens on `slot.myRoomAtLastRecv != roomNo ||
slot.myStageAtLastRecv != stage` (the M2 mutual-deadlock clause). For the
snapshot sender this can open the gate to a remote not in `roomNo` when "our
room changed since their last send". This is **not a correctness bug**: the
relay itself is room-scoped (`SendToAllInRoom` with the `(stage, room)` key,
session.cpp) and the receive side re-gates on the local room, so an over-open
sender gate is fully absorbed — at most bounded wasted bandwidth, never a
mis-delivered snapshot. The consolidation is exactly what the capstone
(`review-full-deepseek` MINOR L) recommended; restated here so M5 doesn't
re-derive it. No fix.

### MINOR 3 (observation — benign, no fix required) — optimistic freeze also fires for `roomNo < 0` (transitional)

**File**: `src/dusk/coop/coop_enemy.cpp:948-965` (`puppetExecute`).

`OwnsRoom(roomNo)` returns false for `roomNo < 0` (`amIRoomOwner` rejects
negative rooms, `coop.cpp:1440`), so a whitelisted enemy briefly in `roomNo < 0`
during a stage transition hits the optimistic-freeze branch and is held for one
frame. This is benign (it re-evaluates next frame once the real room resolves)
and matches the M2.5 "freeze by default" intent. No fix.

### MINOR 4 (observation — low-risk, no fix required) — `RegisterNetVarCallbacks` touches `g_session` from a config-var callback

**File**: `src/dusk/coop/coop.cpp:903-930` (`RegisterNetVarCallbacks`) + the
`onNetVarChange` lambda.

The callback resets `g_startFailed` and, for a `Rejected`/`Ended` session,
calls `g_session.Stop()` + `g_sessionStarted = false`. This assumes config-var
change callbacks fire on the **game thread** (the same thread `onGameFrame`
runs `EnsureSession`/`Stop()` on). In this engine cvar sets are game-thread
(`dusk::config::subscribe` invokes synchronously on set), so there's no data
race on `g_session` / `g_sessionStarted`. If M5 ever moves cvar sets off the
game thread, this callback would need marshalling. No fix now.

---

## VERIFIED-OK (the M4.6 claims, confirmed with real evidence)

### MAJOR 1 — transport teardown (`session.cpp`, `transport.cpp`, `coop.cpp`)

- **`Session::Stop()` now stops the transport whenever running, independent of
  `state_`** (`session.cpp:392-426`): `if (transport_.IsRunning()) { <send
  goodbye if applicable>; transport_.Stop(); }` **before** the
  `if (state_ == Idle || state_ == Ended) return;` guard. The old early-return
  for `Ended` (which skipped `transport_.Stop()`) is gone — the guard now runs
  *after* the teardown, so an `Ended`-with-transport-running session is torn
  down.
- **Message-before-stop ordering is correct.** The goodbye
  (`SessionEnd` for host-`Listening`, `PlayerLeave` for client-`Joined`) is
  enqueued via `SendToAll`/`SendToPeer` (push to the SPSC outbox) **before**
  `transport_.Stop()` joins the socket thread. `Transport::Stop()`
  (`transport.cpp:111-122`) sets `stop_` and joins the thread; the socket
  thread's service loop drains the outbox every iteration
  (`transport.cpp:204-213`) and does a **final `DrainOutbox()`** after the loop
  exits and before `enet_host_destroy` (`transport.cpp:217-219`). So the
  goodbye goes out before the host is destroyed. Verified by reading both
  functions.
- **Both remote-end paths leave `Ended` with the transport running and no
  local `Stop()`**: `HandleDisconnect` (`session.cpp:496-513`, client branch
  sets `Ended`/`ConnectionLost`, comment explicitly notes "no
  `transport_.Stop()` here on purpose") and `OnSessionEnd`
  (`session.cpp:808-815`, sets `Ended`, comment notes the host-side end "must
  remain tearable by a later `Stop()`").
- **The coop glue calls `Stop()` the frame it observes `Ended`**
  (`coop.cpp:1335-1338`): `if (g_session.state() == Ended &&
  g_session.transportRunning()) { g_session.Stop(); }` — inside the
  `g_sessionStarted` block after `Update()`. `g_sessionStarted` stays true
  (no auto-restart); the user re-arms via `net.enabled` / a `net.*` var change
  (MINOR F, `coop.cpp:903-930`).
- **`transportRunning()` exposed** (`session.h:176-178` → `transport_.IsRunning()`,
  `transport.cpp:124-126`) so the glue + selftest can observe the teardown.
- **Selftest is real and non-tautological** (`selftest_main.cpp:2489-2643`,
  `RunM46SessionRestartCheck`):
  - **Leg A (graceful `SessionEnd` / `OnSessionEnd`)**: host+client handshake
    → client `Joined` → assert `transportRunning()` (running while joined) →
    `hostA.Stop()` (sends `SessionEnd`) → client goes `Ended` → **assert
    `transportRunning()` ("STILL running after the remote-side end
    (pre-teardown)")** ← this is the assertion that proves the test is in the
    defect's precondition state, making it non-tautological → `c.Stop()` →
    **assert `!transportRunning()`** (proves `Stop` tore it down for `Ended`)
    → start `hostB` → **`c.StartClient(hostB)` succeeds** → `Joined`,
    `selfId()==1`. On the **old** `Stop()` (early-returned for `Ended`),
    `c.Stop()` would be a no-op, the transport would stay running, and
    `c.StartClient()` would hit `"start client called on an already-running
    transport"` → `false` → the `Check` fails. So the row genuinely
    distinguishes old vs new.
  - **Leg B (hard connection loss / `HandleDisconnect`)**: a raw `Transport`
    acts as a fake host, hands the client a **valid** `JoinAccept` (so it
    reaches `Joined`), then `fakeHost->Stop()` (no `SessionEnd`) → the client's
    ENet peer times out → `HandleDisconnect` → `Ended`(
    `ConnectionLost`) → assert `transportRunning()` (pre-teardown) →
    `c.Stop()` → assert `!transportRunning()` → start `hostC` →
    `c.StartClient(hostC)` succeeds → `Joined`. Exercises the
    `HandleDisconnect` path distinctly from leg A's `OnSessionEnd` path —
    exactly the two paths the capstone MAJOR 1 identified as missing teardown.
    Both legs reuse the **same `Session` object** (`c`) for the second/third
    start, proving the same-process restart on one object.
- **Next `StartClient`/`StartHost` in the same process succeeds**:
  `StartClient`/`StartHost` reset `state_ = Idle`, `selfId_`, and
  `startFailureReason_` (`session.cpp:316-318`, `:363-365`) then call `Stop()`
  (no-op once the transport is down) and `transport_.StartClient/StartHost`
  (`host_ == nullptr` after the prior `Stop()` destroyed it) → succeeds.

### MAJOR 2 — PollHostDeaths UAF (`coop_enemy.cpp`)

- **`e.adapter` captured at `RegisterActor`** (`coop_enemy.cpp:446-455`):
  `const NetEnemyAdapter* adapter = AdapterForActor(actor); e.adapter =
  adapter;` — `AdapterForActor` is a lookup into the **static `kAdapters`
  `std::array`** (`coop_enemy.cpp` `findAdapter`), which outlives every entry,
  so the pointer is safe to hold for the entry's whole lifetime. The struct
  member is documented (`coop_enemy.cpp:283-292`).
- **`PollHostDeaths` never re-derives the adapter from `e.actor`**
  (`coop_enemy.cpp:528-560`): the top-of-loop `AdapterForActor(e.actor)` is
  **gone**; the `gone` branch (`fopAcM_SearchByID(e.pid) != e.actor`) pushes
  to `dead` and `continue`s with **no `e.actor` deref**; the death-window reads
  use `e.adapter->isDead(e.actor)` / `e.adapter->deathSwitchNo(e.actor)`
  (`:546-547`), reached **only when `!gone`** (the actor is alive then,
  `fopAcM_SearchByID(e.pid) == e.actor`); the dead-resolution loop uses
  `adapter = e.adapter` and `d.drop = adapter->dropTableId` (`:555-556`) —
  **`dropTableId` comes from `e.adapter`, not the actor**. `e.deathSwitch` was
  captured while the actor still lived (`hostOnExecuted`, post-execute). So
  **no `e.actor` deref ever happens after the actor is freed.**
- The remaining `AdapterForActor(e.actor)` call sites (`RegisterActor`,
  `PackAnim`, `SendSnapshot`) all run while the actor is alive (registration
  is post-create; the snapshot sender is post-execute) — safe.

### MAJOR 3 — freeze-latency window (`coop_enemy.cpp`)

- **`ScanAndRegister()` runs immediately on room change** (`coop_enemy.cpp:1233-1247`):
  the room-change block `if (stageChanged || roomNow != g_lastRoom) { ...
  ClearAll(); ... ScanAndRegister(); }` calls `ScanAndRegister()` **outside**
  the `% 15` cadence — so local instances are registered (and thus frozen by
  `puppetExecute`) on the **first frame** in the new room. The 15-frame
  cadence stays as a backstop for mid-room spawns
  (`coop_enemy.cpp:1257,1266` — `if (g_frameCount % 15 == 0) ScanAndRegister();`
  in both the owner and non-owner branches).
- **Optimistic freeze** (`coop_enemy.cpp:948-965`): `if (OwnsRoom(roomNo))
  return false;` (owner sims natively — checked **first**) → `FindEntryForActor`;
  if `nullptr` and `isWhitelistedType(name)` → `actor->old = actor->current;
  return true;` (hold the spawn pose until the first snapshot lands). A
  non-whitelisted type still `return false` (native AI — correct: non-whitelisted
  enemies aren't coop-managed). This closes the window where a non-owner's
  local whitelisted enemy ran full native AI (real damage to the client's Link,
  bypassing the `CombatIntent` path) for up to ~15 frames after room entry.
- **Owner's own room still sims natively** — `OwnsRoom(roomNo)` returns
  `false` from `puppetExecute` **before** the optimistic freeze, so the
  owner's whitelisted enemies are never optimistically frozen. Verified
  (`amIRoomOwner`, `coop.cpp:1439-1447`).
- **No regression to M2/M2.5 freeze/apply**: the `puppetExecute` path after
  `FindEntryForActor` returns non-null is unchanged (`Dying` hold-pose,
  no-snapshot hold-pose, `AdapterForActor` + apply `snap.pos/angle/...`,
  per-entry `cullMtx`). The only change is the `e == nullptr` branch. The
  15-frame backstop, `PollHostDeaths`, and `combat::flushHostIntents` cadence
  are unchanged.

### MINOR cluster (all verified)

- **SceneChange receive gate (MINOR A)** — `coop.cpp:298-303`: `if (ev.scene ==
  1) { slot.state.roomNo = ...; }` — adoption gated on the same-stage flag,
  matching the host's room-table sniff (`session.cpp`). A cross-stage mover
  (`scene=0`) no longer overwrites `roomNo` while `stage` stays the old stage.
- **Spawn-limiter reset (MINOR G)** — `coop.cpp:735-752` (anchor capture on
  deadline abort) + `:788-810` (reset the `dropped` limiter when the host
  moved >300 units or changed room from the stuck anchor). The M3 "3 strikes"
  budget is now position-specific, not permanent — a host at a cliff edge no
  longer hides the remote's puppet forever.
- **Sender-gate consolidation (MINOR L)** — `coop.cpp:418-441`:
  `RemoteInOurRoom(const char* stage, s8 roomNo)` is now the **single**
  implementation; `sendPlayerState` calls it (`:1233`) and `remoteInRoom`
  delegates to it (`:1449-1457`). Semantics preserved (the `!hasState`
  unknown-opens, same-stage-same-room, and my-room-changed clauses); the
  snapshot-sender over-open is absorbed by the relay + receive scoping (MINOR 2
  above). The two implementations can no longer drift.
- **`onRoomUnload` deleted (MINOR C)** — removed from `coop_enemy.h` (was
  `:147`) and `coop_enemy.cpp`; `grep -rn onRoomUnload src/ include/` returns
  nothing. No remaining caller.
- **Per-stage reset keyed on `(stage, room)` (MINOR D)** — `coop_enemy.cpp:357-365`
  (`g_lastStage[net::kMaxStageNameLength]`) + `:1233-1244` (`stageChanged =
  strcmp(stageNow, g_lastStage)`, reset on `stageChanged || roomNow !=
  g_lastRoom`). `localStageName()` exported (`coop.h:107`, `coop.cpp:1419`).
  A stage change with a coincident room number now resets explicitly (no
  reliance on the implicit Link-less gap).
- **Join/start-failure feedback (MINOR F)** — `session.cpp:336,382`
  (`startFailureReason_ = transport_.LastStartError()`) + the distinct log
  (`:334-335`, `:381`); `transport.cpp` `LastStartError()` with static reason
  strings (`:29-92`, "transport already running" / "listen failed (port
  busy...)" / "cannot resolve join host" / "connect failed"); `coop.cpp:960-981`
  (distinct failure log + one-shot `NotifyCoop` on first failure);
  `coop.cpp:1305-1313` (one-shot `NotifyCoop` on `Rejected`); `coop.cpp:903-930`
  (`RegisterNetVarCallbacks` — reset `g_startFailed` + re-arm an
  `Ended`/`Rejected` session when `net.*` vars change). The 3 s retry spam is
  now a single toast + "(retry)" log.
- **Synthetic-collider hardening (MINOR H)** — `coop_enemy.cpp:337`:
  `g_entries` is `std::map` (node-based — stable addresses across inserts);
  the `synthAt`/`synthStts` members carry the same-frame consumption contract
  comment (`:295-307`). `SetTgHitSynthetic`'s `&e.synthAt` / `&e.synthStts` no
  longer dangle on a rehash.
- **`g_roomClear < 128` (glm MINOR 7)** — `coop_enemy.cpp:1139`: `if
  (ev.enemyId < 128 && ...)` (was `< 256`); the arrays stay 256 (2× the
  meaningful s8 range), documented (`:353-356`).
- **`sendPlayerState` isPuppet guard (glm MINOR 6)** — `coop.cpp:1198-1200`:
  `if (isPuppet(link)) return;` at the top of the sender — defense-in-depth
  (the puppet `execute` returns before the call site today, but the sender no
  longer relies on that ordering).
- **`OnWorldInit` comment (glm MINOR 8)** — `session.cpp:817-826`: documents
  that `worldStage_` on a client is a join-time reference only (no client
  consumer reads it; the puppet gate keys on the remote's real
  `PlayerState.stage`); the unconditional write stays by design.
- **Version-history note (glm MINOR 4)** — `protocol.h:70-76`: the
  `kProtocolVersion = 6` comment now records that M4.5 redefined
  `PlayerEventMsg.scene` as the same-stage flag (semantic only, no layout
  change; M5 bumps to v7). No bump — correct (no layout change).
- **Dead-wire doc comments (glm MINOR 2)** — `protocol.h:373-379`:
  `EnemySnapshotMsg.type` and `.aggro` carry "populated on the host,
  serialized, INTENTIONALLY never read by the client apply path in v1 — M5
  decides wire-vs-drop"; `coop.cpp:311-315`: the `CombatResult` receive branch
  is documented as intentionally unconsumed dead traffic in v1.
- **Docs (MINOR I + glm MINOR 1/3)** — 2017-B `PlayerState`: `protocol.cpp:166-172`
  (the `WireSize` comment now sums to 2017 with the 16-byte stage field),
  `protocol.h:95-98` (`kMaxMessageSize` "~2.0 KB — 2017 B payload + 4 B
  envelope"), `00-network.md` §5 (`:192`, "= 2017 bytes") + §6
  (`:273-278`, "8 × 2017 B × 60 ≈ 0.97 MB/s"), `02-player-state.md` §2
  (`:165-179`, "2017 B with the 16-B stage field"); 44-B `EnemySnapshot`:
  `00-network.md` §5 (`:246-249`, "= 44 bytes/enemy/frame") + §6
  (`:275-276`, "40 × 44 B × 60 ≈ 106 KB/s"), `protocol.cpp:174-176` (= 44);
  04 byte count: `04-time-weather.md` `TimeEvent` 10 → **12** (8 payload + 4
  envelope); supersede banners: `03-enemies.md` §3.4 (`:235-241`) + §7.1
  (`:498-503`) — "SUPERSEDED — see m2-design-notes.md §2/§5" (E_FB is the
  Freezard, dropped; E_GS dropped; shipped whitelist `E_AI, E_HM, E_DF, E_YC,
  E_MD, B_TN`); `TESTING.md` + `implementation-plan.md` status blocks updated
  (318 → 361, M4.6 block with the MAJOR/MINOR inventory).

### Selftest additions (MINOR K + the MAJOR rows) — real, would fail on the old behavior

- **`RunM46SessionRestartCheck` (`selftest_main.cpp:2489-2643`)** — the
  MAJOR-1 rows (both legs, above). Non-tautological: the
  `transportRunning()`-after-remote-end assertion pins the precondition; the
  old `Stop()` would leave the transport running and the next `StartClient`
  would fail the `Check`.
- **RoomClear + cross-stage EnemyEvent rows (`selftest_main.cpp:2229-2262`)** —
  owner B sends `EnemyEvent(RoomClear)` for room 2 while A is in room 1 →
  `aEvents == 0` (receive-gate no-op); then A moves to `(F_SP104, 2)` (a
  stage whose room number coincides with B's `F_SP108` room 2), B sends
  `EnemyEvent(Died)` for a `F_SP108` room-2 enemy → `aEvents == 0` (the
  **stage** half of the `(stage, room)` relay key blocks the cross-stage
  mis-kill). The cross-stage row locks the M4.5 MAJOR-2 fix exactly — on the
  pre-M4.5 star-relay, A's coincident `(2<<8)|setID` local enemy would have
  been mis-killed with the wrong drop/switch.
- **The `unique_ptr<Session>` refactor** across the demo functions
  (`RunHandshakeDemo`, `RunGameMessageDemo`, `RunM2RelayPolicyCheck`,
  `RunM3TimeWeatherCheck`, `RunM35TimeWeatherFixCheck`, `RunGenerationGuardCheck`,
  `RunDuplicateJoinCheck`, `RunJoinAcceptValidationCheck`, `RunOversizedMetricCheck`,
  `RunM4OwnershipTableCheck`, `RunM4RoomRoutingCheck`, `RunM4WorldStageCheck`,
  `RunM4EntityStabilityCheck`) is mechanical (`Session` → `unique_ptr<Session>`,
  `&x` → `x.get()`, `x.m` → `x->m`) and is what makes the same-process restart
  exercisable (a stack `Session` in `RunHandshakeDemo` can't be restarted after
  scope exit; the restart test owns its `Session` on the heap). No behavior
  change to the existing rows.

### Regression — clean

- **Full `dusklight` rebuild**: touched every net/coop TU + header → clean
  compile + link, 33903 exports / 2053 objects, exit 0. Only the pre-existing
  vanilla `d_a_alink.cpp`/JStudio deprecation warnings + the `ld: ignoring
  duplicate libraries` note. Zero net/coop TU warnings.
- **Selftest**: 361/361 PASS, exit 0, twice (stable — the peer-timeout leg is
  timing-sensitive).
- **Net-off boot untouched**: no `d/`/`f_`/`libs/` file changed; every M4.6
  path is `g_sessionStarted`/`SessionLive()`/`state()`-gated.
- **No new dead code**: every new addition is wired — `NotifyCoop`
  (`coop.cpp:192/980/996/1019/1024/1312`), `startFailureReason_`
  (`session.cpp:318/336/365/382`, read `coop.cpp:969`), `transportRunning()`
  (`coop.cpp:1336`, selftest), `LastStartError()` (`transport.cpp`,
  `session.cpp:335/381`), `SweepStaleHostEntries` (`session.cpp:1016`),
  `localStageName()` (`coop_enemy.cpp:1235`), the `CreateLimiter` anchor fields
  (`coop.cpp:740-810`). `grep -rn onRoomUnload src/ include/` = empty.
- **No M5 features**: the changes are fixes + comments + the restart/RoomClear
  selftest rows. No waves, no horses, no PvP, no interpolation, no spawn-params
  wire. `kProtocolVersion` unchanged (6).

---

## MUST-FIX

**None.** M4.6 resolves all 3 MAJORs and the full MINOR cluster; HEAD is
buildable, selftest-green (361/361, twice), regression-clean (full `dusklight`
link clean; no vanilla file touched; net-off untouched), and **READY for the
final live playtest**. The MINOR observations above are benign (most are the
explicit capstone recommendations) and require no fix before the playtest.

The one thing the selftest cannot cover is the **live** two-instance playtest
(the RVZ is gitignored) — in particular the live MAJOR-2 enemy-death probe
(the UAF is latent under a normal heap and won't crash live, but an
ASan/fill-pattern build would) and the live MAJOR-3 room-entry window (watch
the first ~1 frame after a non-owner room entry for enemies moving/attacking
before the freeze). Both are fixed in code and selftested at the session layer;
the live run is the user's final confirmation.
