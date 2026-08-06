# M4.6 CAPSTONE FIX-PASS REVIEW — adversarial verification of the 3 MAJORs + MINOR cluster (deepseek-v4-flash-0731)

Review range: `cfdf2cc598..HEAD` (`aca524df3967`), branch `net-coop`.
Reviews under test: `review-full-deepseek-v4-flash-0731.md` (MAJOR 1, MINORs A/C/D/F/G/H/I/J/K/L),
`review-full-glm-5.2.md` (MAJOR 1 UAF, MAJOR 2 freeze-latency, MINORs 1–8).
Aurora submodule at upstream `6c4c27f9e8e4` — untouched, not a regression.

## Evidence gathered (all real, this environment)

- **Forced rebuild of every touched TU + full game link**: re-touched
  `session.cpp`, `transport.cpp`, `protocol.cpp`, `discovery.cpp`,
  `selftest_main.cpp`, `coop.cpp`, `coop_enemy.cpp` → `ninja dusklight
  dusk_net_selftest` → clean compile+link of the full game
  (`Dusklight.app/Contents/MacOS/Dusklight`, 48.6 MB). **Zero warnings from
  the touched TUs**; the only warning in the entire rebuild is the known
  pre-existing vanilla `JKRExpHeap.h:30` multichar constant (JSystem
  transitive include, untouched by the net work).
- **Selftest**: `./dusk_net_selftest` × 3 → **`PASS: all checks succeeded`,
  361 `ok` checks (was 318), exit 0** (~16.6 s — leg B deliberately waits out
  the ENet peer timeout). All 18+ suites green including the new
  `RunM46SessionRestartCheck` (both legs), the M4.6 RoomClear receive-gate
  rows, and the explicit cross-stage EnemyEvent row.
- **Git hygiene**: working tree clean (only this review is written); no source
  files modified.
- **Live loopback not runnable here** (TP RVZ gitignored) — anything touching
  the live engine (puppet create at real cliff edges, freeze visuals, the
  in-app Host-leave → rejoin UX) is source-verified, not live-verified. Same
  limitation as every review since M4.

---

## OVERALL VERDICT

**M4.6 resolves all 3 MAJORs and the entire MINOR cluster.** HEAD is
buildable (forced rebuild of every touched TU, zero warnings, full game link
clean), selftest-green (361/361 × 3), regression-clean (every M1–M4.5 suite
still green; net-off boot untouched; no M5 features), and **READY for the
final live playtest**. The three fixes are real, targeted, and — critically —
each is *verified* (selftest rows that fail on the old behavior, or
source-level proof of the specific invariant the review demanded):

- **MAJOR 1 (transport teardown)** — fixed at both layers (session + glue) and
  selftested with a test that provably fails pre-fix (asserts the transport is
  still running after a remote-side end, then asserts `Stop()` tears it down
  and a second/third session reaches `Joined` in the same process).
- **MAJOR 2 (PollHostDeaths UAF)** — fixed exactly as prescribed (`e.adapter`
  captured at registration; `PollHostDeaths` derefs `e.actor` only when
  `!gone`); I grepped every `e.actor` read in the TU and each remaining one
  provably runs on a live actor.
- **MAJOR 3 (freeze-latency window)** — closed twice over (immediate
  `ScanAndRegister` on room change + optimistic freeze), with the owner's own
  room provably still native (the `OwnsRoom` check precedes the freeze) and
  the 15-frame backstop retained.

**BLOCKER: none. MAJOR: none found.** The findings below are informational
notes (documented deferrals and behavior notes), none of which block the
playtest.

---

## RANKED FINDINGS

### BLOCKER — none.

### MAJOR — none. (All three capstone MAJORs verified fixed, below.)

### MINOR / informational

**M46-N1 (info — documented deferral, not a defect): MINOR B (aggro fallback)
and MINOR E (version bump) are OUT of the M4.6 fix scope by design, and both
are now documented on the wire.**
- `aggroId = 0; // the host's own Link` is still the fallback in
  `SendSnapshot` (`coop_enemy.cpp:666-668`) — on a client-owned room the byte
  should be the owner's `selfId()`. It stays dormant (no receive-side consumer
  in v1; `protocol.h` EnemySnapshotMsg comment now says "M5 decides
  wire-vs-drop"). The task's fix list excludes MINOR B; `protocol.h:373-377`
  and `protocol.h:70-77` (kProtocolVersion) document both as M5 actions.
  **Fix: at M5 start (the v7 bump), before any aggro consumer exists.**

**M46-N2 (note — the M4.6 RoomClear/cross-stage rows are coverage-locks with
a real regression-catch, and the receive-side gate is source-only):** the
RoomClear relay rows (selftest 2c(a)/"lands" row) and the cross-stage
EnemyEvent row (2c(b)) would fail on the pre-M4.5 star-relay (aEvents would
be ≥1 where the row asserts 0) — they genuinely lock the M4.5 MAJOR-2 relay
scoping. The *receive-side* gate in `coop_enemy.cpp` (`ev.enemyId < 128 &&
(s8)ev.enemyId == localRoomNo()` for RoomClear; the same for Died/snapshots)
is game-side (not linked into the selftest) — verified by source, not by
row. The < 128 tightening (MINOR 7) is correct: room numbers are s8 0..127
in practice; a forged 128..255 can no longer write the (oversized-by-2×)
arrays.

**M46-N3 (note — sender-gate consolidation is behavior-preserving for the
enemy-snapshot sender):** `remoteInRoom` now delegates to the single
`RemoteInOurRoom(stage, roomNo)` (`coop.cpp:1453`). Re-derived: `myStageAtLastRecv`/
`myRoomAtLastRecv` are *our* stage/room at the remote's last state, so a
cross-stage remote still gates closed whenever our own room is unchanged
(identical to the old `continue`-based rule). The only sends the new gate
adds are in the "owner recently moved rooms" window; those are dropped by the
receive-side room gate (bandwidth-only, bounded by player count).

**M46-N4 (note — MINOR G limiter reset is a 300-unit heuristic):** the
strike budget resets when the host's room changed, the real Link vanished
(stage transition), or position moved > 300 units (≈ 3 m) from the stuck
anchor (`coop.cpp:790-802`). Satisfies the review's "reset when the host's
position/room changes materially" — a host who backs up < 3 m from the exact
cliff edge stays dropped (retry requires material movement). Acceptable for
v1; a fallback-spawn retry (review's alternative) is the M5 hardening.

**M46-N5 (note — `SweepStaleHostEntries` is host-only by construction):**
`setLocalRoom` → sweep runs only on the host (`coop.cpp:1358` is
`hostRole()`-gated; the selftest uses a host `Session`). On the host every
entry has a real order list (`SetOwner`-created `orderLen==0` entries exist
only via `RoomOwnershipMsg`, which hosts never receive), so the sweep cannot
erase a legitimate entry; the sticky client-owned room (`owner != 0`) and the
host's own room (`owner==0, host in order`) both survive. The client's
`orderLen==0` minimal view is never swept (no client call site).

**M46-N6 (info — the M4.6 start-failure/join-reject toasts + net.* var
re-arm introduce no recursion hazard):** `dusk::config::subscribe` fires only
on `notify_changed` (no immediate callback), copies the subscription list
before invoking, and guards recursive same-name notifications
(`config.cpp:585-631`). The callback only touches Rejected/Ended sessions and
`g_startFailed`; an active session is never disturbed by a var edit.

---

## THE THREE MAJORS — verified, with evidence

### MAJOR 1 — session teardown on a remote-side end (`transportRunning` + unconditional transport stop + glue + selftest)

**Fix is present and complete:**
- `Session::Stop()` now tears the transport down whenever `transport_.IsRunning()`,
  independent of `state_`, with the goodbye messages enqueued *before* the
  teardown (`session.cpp:392-425`). The leave/end messages go out because
  `Transport::Stop()` joins the socket thread and `SocketThreadMain` runs a
  **final `DrainOutbox()` + `enet_host_flush` before `enet_host_destroy`**
  (`transport.cpp:205-216`) — the join returns only after the drain.
- `HandleDisconnect` (`session.cpp:506-513`) and `OnSessionEnd`
  (`session.cpp:809-816`) still leave the transport running (by design) — the
  glue half is `coop.cpp:1325-1337`: `if (state == Ended &&
  transportRunning()) g_session.Stop();` the frame the end is observed.
- `StartHost`/`StartClient` still begin with `Stop()` (now effective for an
  Ended session), so the next session in the same process starts clean.
- **The selftest is NOT tautological** — `RunM46SessionRestartCheck`
  (`selftest_main.cpp:2489-2643`):
  - Leg A: host+client handshake → host `Stop()` (SessionEnd path) → client
    asserts `transportRunning()` is **still true** after the remote-side end
    (the pre-fix dead-end state) → `Stop()` → asserts **false** → second
    `StartClient` against a new host → **`Joined`** with a re-assigned id.
  - Leg B: a raw `Transport` fake-host answers the JoinRequest with a valid
    JoinAccept → client reaches `Joined` → fake host's transport dies with no
    SessionEnd → client's ENet peer timeout fires `HandleDisconnect` →
    `Ended` with `endReason == ConnectionLost` → same teardown + **third**
    session reaches `Joined`.
  - Pre-fix, `Stop()` early-returned for `Ended`, so the
    "Stop() stopped the transport after a remote-side end" row and both
    restart rows would fail. Verified green: all 26 rows, exit 0.

### MAJOR 2 — PollHostDeaths use-after-free (adapter captured at registration)

**Fix is present and complete.** `RegisterActor` stores
`e.adapter = AdapterForActor(actor)` at registration (`coop_enemy.cpp:449-455`);
`kAdapters` is a `static std::array` (lives forever — safe to hold).
`PollHostDeaths` (`coop_enemy.cpp:520-560`):
- the top-of-loop `AdapterForActor(e.actor)` is gone;
- `isDead`/`deathSwitchNo` read `e.adapter` and are reached only when
  `!gone` (`fopAcM_SearchByID(e.pid) == e.actor` — the actor is alive);
- the gone/dead-resolution loop reads `e.adapter->dropTableId` and the
  already-captured `e.deathSwitch` — **zero `e.actor` derefs after `gone`**.

Grep of every remaining `e.actor` use in the TU, each provably on a live
actor: `SendSnapshot` (owner post-execute), `SetTgHitSynthetic`
(`e.synthStts.SetActor(attacker ?: e.actor)` — host pre-execute),
`applyInjectedHit`, `OnEnemyDied` (client; `Dying`-state guarded, and
messages are processed before the same frame's dying-delete pass, which
erases the entry at delete), and the dying-delete pass itself (`fopAcM_delete`
then immediate erase — the queued free happens after the entry is gone).

### MAJOR 3 — freeze-latency authority window (immediate scan + optimistic freeze)

**Fix is present and complete.**
- `ScanAndRegister()` runs IMMEDIATELY on room/stage change
  (`coop_enemy.cpp:1240-1250`), not gated on `% 15`; the 15-frame cadence
  remains in both the owner and non-owner branches as the mid-room-spawn
  backstop (`:1256`, `:1262`).
- `puppetExecute` (`coop_enemy.cpp:942-966`): the `OwnsRoom(roomNo)` check
  comes FIRST, so the **owner's own room sims natively** (host: room
  published via `setLocalRoom` before `enemy::onGameFrame` each frame —
  `coop.cpp:1358`; client: RoomOwnershipMsg view). Only whitelisted,
  non-owned, unregistered types take the optimistic freeze
  (`actor->old = actor->current; return true;`); non-whitelisted types still
  run native (unchanged M2 semantics).
- Registered path (snapshot apply / `driveModel` / `refreshColliders`) is
  byte-for-byte the M2/M2.5 code — no freeze/apply regression.
- Ownerless-room analysis: a room a player is *in* always has an owner
  (first-in / host-default / transfer-on-leave), so the freeze-until-snapshot
  never starves a visible room; the transient pre-ownership window is the
  documented "statue ~1 RTT" behavior.
- Room-change reset now keys on **(stage, room)** via `g_lastStage`
  (`coop_enemy.cpp:1225-1240`, MINOR D) — a stage change with a coincident
  room number no longer relies on the implicit Link-less gap; `g_lastStage`
  staleness across a dead session is safe (the `g_lastRoom == -2` guard forces
  the reset block on the first live frame).

---

## VERIFIED-OK

1. **MAJOR 1 (session teardown)** — fix + glue + non-tautological selftest (26 rows, both the graceful SessionEnd and the HandleDisconnect peer-timeout legs). Messages-before-join ordering source-verified (final outbox drain precedes `enet_host_destroy`).
2. **MAJOR 2 (PollHostDeaths UAF)** — `e.adapter` at registration; no `e.actor` deref after `gone`; `dropTableId` from `e.adapter`; all other `e.actor` reads provably live.
3. **MAJOR 3 (freeze latency)** — immediate scan + optimistic freeze; owner-native preserved; 15-frame backstop intact; M2/M2.5 apply path untouched; per-stage reset keyed on (stage, room).
4. **MINOR A** — SceneChange receive gate `ev.scene == 1` (`coop.cpp:300`); send side sets scene=1 only for same-stage moves (`coop.cpp:1227-1229`); cross-stage moves established by PlayerState alone.
5. **MINOR C** — `onRoomUnload` deleted from `.cpp` and `.h`; zero callers remain (grep: only doc references).
6. **MINOR D** — per-stage reset keyed on `(localStageName(), room)`; `localStageName()` exported and wired.
7. **MINOR F** — one-shot start-failure toast + distinct cause logging (`startFailureReason`/`LastStartError`: port-busy vs already-running vs resolve-failed vs connect-failed); one-shot join-reject toast; failed-start + Rejected/Ended re-arm on any net.* var change; no recursion hazard (config.cpp subscription semantics verified).
8. **MINOR G** — spawn-limiter strike budget resets on material movement (room change, Link loss, >300 units); anchor captured at each abort.
9. **MINOR H** — `g_entries` is now node-based `std::map` (stable entry addresses); same-frame consumption contract documented on `synthAt`/`synthStts`.
10. **MINOR I / glm MINOR 1** — 2017-B PlayerState and 44-B EnemySnapshot corrected in `protocol.h`/`protocol.cpp` comments, 00-network §5/§6 (0.97 MB/s + 106 KB/s), 02 §2, plan §9 table; `kMaxMessageSize` comment → ~2.0 KB; 04 §4 TimeEvent → 12 B; no stale "2001"/"~28 B"/"2.7 KB"/"players 1/{}" strings anywhere (grep).
11. **MINOR J** — announcer log reads live `players_.load()` (`discovery.cpp:174-178`).
12. **MINOR K** — RoomClear relay rows (no-op from room 1, lands from room 2) + explicit cross-stage EnemyEvent row with coincident room numbers — all would fail on the pre-M4.5 star-relay; green.
13. **MINOR L** — one sender gate (`RemoteInOurRoom(stage, roomNo)`); both senders delegate; behavior-preserving (N3).
14. **glm MINOR 2** — CombatResult / `aggro` / `type` dead-wire documented on the wire and at the receive no-op (behavior unchanged).
15. **glm MINOR 3** — 03-enemies.md §3.4/§7.1 supersede banners → m2-design-notes §2/§5; E_FB/E_GS corrected, shipped whitelist restated.
16. **glm MINOR 4** — v6 history comment records the `PlayerEventMsg.scene` semantic (no bump; M5 → v7 documented).
17. **glm MINOR 5** — `SweepStaleHostEntries` on host `setLocalRoom`; host-only, safe on every entry class (N5).
18. **glm MINOR 6** — `sendPlayerState` isPuppet guard (`coop.cpp:1197-1201`).
19. **glm MINOR 7** — RoomClear receive gate `< 128`; arrays documented as 2× the s8 range.
20. **glm MINOR 8** — OnWorldInit comment documents `worldStage_` as join-time reference on clients.
21. **Regression** — full game link clean after touching every net/coop TU; selftest 361/361 × 3 (all M1–M4.5 suites intact); net-off vanilla untouched (all new glue inside `onGameFrame`/`EnsureSession`, session-gated); Aurora at upstream; no M5 features; no new dead code (all new APIs have callers).
22. **Docs** — TESTING.md status block (361 checks), plan status blocks + commit list, supersede banners all match HEAD.

---

## TOP N THINGS FOR THE FINAL LIVE PLAYTEST (the selftest cannot cover these)

1. **Host-leave → rejoin in the same process, in-app**: host quits (graceful, then a hard `kill`) → client toasts "Host left"/"Host disconnected" → without restarting, start a new session from the Network tab (and via a net.* var edit) → must succeed (was: "already-running transport" forever). Also the graceful client-initiated leave → rejoin.
2. **Room-entry freeze (MAJOR 3, live)**: client walks into a host-owned room with E_AI/E_YC/B_TN → enemies must statue from the FIRST frame (no native-AI damage window), snap to the host's pose, and the client's Link must take no heart damage in the entry window. Host's own room must still sim natively (enemies move/attack on the host immediately).
3. **Host kills enemies of every whitelist type (MAJOR 2, live)**: drops + save switches land on both machines; run a sanitised build if possible — the pre-fix UB is gone by construction, but the live kill path is the final probe.
4. **Cliff-edge puppet (MINOR G, live)**: host stands at a cliff edge ≥ 30 s with a remote joined → puppet dropped; walk ≥ 3 m away → puppet must spawn on the next retry.
5. **Join-reject / start-failure UX (MINOR F, live)**: join a full or version-mismatched session → one toast, then edit any net.* var → clean re-arm; start a host on a port already in use → distinct "port busy" toast once (no 3 s log spam).
6. **Cross-stage kill isolation (MAJOR 2 fix, live)**: host in F_SP103 room 1, client in F_SP104 room 1 → host kills a whitelisted enemy → client's coincident (1<<8)|setID enemy must not die, no wrong drop/switch, ALLDIE intact. Also: client in room 1 (same stage) must still get the died event + RoomClear bit.
7. **Time/weather + save integrity + net-off baseline** (unchanged from the capstone list): sky/rain sync, stage-transition re-assert, `USA/Card A/*.gci` mtime untouched, then boot with no cvars → vanilla single-player.

## MUST-FIX

**None.** (Expected: none — confirmed.)

*Review file naming note: this is `review-m46-<model>.md` per the capstone
convention. No source files were modified; the only write is this review and
gitignored build artifacts.*
