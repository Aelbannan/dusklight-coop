# CAPSTONE ADVERSARIAL REVIEW — full networked co-op implementation (deepseek-v4-flash-0731)

Review range: the whole `net-coop` branch (`git log --reverse 95608438c1..HEAD`, 71 commits,
25886 insertions). This is the **holistic pass** after nine per-milestone reviews
(M0..M4.5, all approved): it attacks the sum of the parts — end-to-end lifecycle, cross-module
interplay, protocol/doc consistency across the entire surface, guard hygiene everywhere,
robustness at every entry point, duplication/dead code across modules, and M5 seams.

Method: read the full decision + review trail (`docs/design/network.md`,
`docs/design/mod-coop/00..04`, `implementation-plan.md` Rev 3, `m2-design-notes.md`,
TESTING.md, reviews m0..m45), then every net/coop source file and every `#if TARGET_PC`
attachment point in vanilla files, then forced-clean builds + selftest runs.

## Evidence gathered (all real, this environment)

- **Forced clean rebuild of the game**: `ninja -t clean dusklight` (2248 files) → `ninja
  dusklight` → clean compile+link, **33903 exports / 2053 objects**, 2m02s. Only the
  pre-existing `ld: ignoring duplicate libraries` note. Re-touching every net/coop TU
  (`coop.cpp`, `coop_enemy.cpp`, `coop_combat.cpp`, `coop_time.cpp`, `coop_context.cpp`,
  `session.cpp`, `transport.cpp`, `protocol.cpp`, `discovery.cpp`, `selftest_main.cpp`) and
  rebuilding: **zero warnings from the touched TUs** (the only warning is the known
  pre-existing `JKRExpHeap.h:30` multichar constant, a JSystem transitive include).
- **Forced clean rebuild of the selftest** (`ninja -t clean dusk_net_selftest` → `ninja
  dusk_net_selftest`) → `./dusk_net_selftest` → **`PASS: all checks succeeded`, 318 checks**,
  verified twice.
- **Protocol math verified by hand**: every `WireSize` matches its serializer field-for-field
  (JoinRequest 40, JoinAccept 326, WorldInit 322, PlayerState **2017** via
  `PlayerStateWireSize()`, PlayerEvent 12, EnemySnapshot 44, EnemyEvent 10, CombatIntent 42,
  CombatResult 12, TimeSync 8, TimeEvent 8, WeatherChange 6, RoomOwnership 20); the
  `jointCount > kMaxJoints` semantic rejection at parse is present.
- **Live loopback not runnable**: the TP RVZ is gitignored (`c4ca64a356`), so no live
  two-instance run is possible here — the final live playtest is the user's. The M4-era
  `net-host.log`/`net-client.log` in the tree are gitignored pre-M4 leftovers, not evidence.
  Everything below that touches the live engine (spawn offsets, `fopAcM_GetRoomNo` values,
  form swaps) is source-verified, not live-verified.

---

## OVERALL VERDICT

**The implementation is coherent, buildable, selftest-green, and regression-clean — ready for
the final live playtest and for M5 development, with one MAJOR lifecycle defect that the
per-milestone reviews structurally could not see (it lives in the join of the session state
machine and the transport lifecycle, on a path no milestone acceptance test exercised) and a
cluster of smaller cross-module gaps.**

- **Coherent**: the architecture matches the docs end to end — star relay, room-scoped
  enemy traffic, stay-put join, host-authoritative time/weather, owner-authoritative enemies.
  The relay policies, sender gates, and receive-side gates agree with each other everywhere I
  traced them (the M4/M4.5 room-scoping work holds up under adversarial re-derivation).
- **Buildable**: forced clean rebuild green, touched TUs warning-free.
- **Selftest-green**: 318/318, twice.
- **Net-off vanilla**: every attachment point is session/isPuppet-gated; the single
  non-gated PC change (the `d_attention.cpp` ALINK skip) is behavior-neutral in single-player
  by construction (only one ALINK can exist without a session).
- **One MAJOR**: a client whose session ends from the host's side (SessionEnd or connection
  loss) never tears down its ENet transport; the next session start in the same process is
  impossible until an app restart. This breaks the "host-leave → continue → rejoin" leg of
  the end-to-end lifecycle (attack category 1) — exactly the kind of sum-of-the-parts defect
  a per-milestone review misses.
- **Nothing else rises above MINOR.** The M4.5 MINORs that were flagged as optional (RoomClear
  selftest rows, the receive-gate comment, the stale announcer log) are confirmed still open
  and are folded into this review.

**BLOCKER: none.** (Build, link, selftest all pass; the co-located playtest path is fully
exercised by the selftest and the code.)

---

## RANKED FINDINGS

### MAJOR 1 — A client whose session ends from the host's side never stops its transport; every later session start in the same process fails forever ("already-running transport"). (cross-module: session state machine × transport lifecycle × coop glue)

**Evidence:**
- `Session::HandleDisconnect` (client branch): `endReason_ = SessionEndReason::ConnectionLost;
  state_ = SessionState::Ended;` — **no `transport_.Stop()`** (`session.cpp:457-460`).
- `Session::OnSessionEnd` (client): `state_ = SessionState::Ended;` — **no
  `transport_.Stop()`** (`session.cpp:753-756`).
- `Session::Stop()` early-returns for `Idle || Ended` (`session.cpp:353-356`), so neither the
  disable path (`EnsureSession`, `coop.cpp:820-828`) nor `shutdown()` (`coop.cpp:1215-1224`)
  can tear the transport down afterwards — `Stop()` is the *only* caller of
  `transport_.Stop()` (`session.cpp:371`).
- The socket thread + ENet host therefore run for the rest of the process. The next
  `StartClient`/`StartHost` (`session.cpp:321-326` / `:280-285`) calls `Stop()` (no-op),
  then `transport_.StartClient` hits `host_ != nullptr` → `"start client called on an
  already-running transport"` → returns false (`transport.cpp:61-64`; host variant
  `:29-32`). `EnsureSession` marks `g_startFailed` and retries every 180 frames forever
  (`coop.cpp:851-855` + `:1178-1180`), logging an error every ~3 s.
- **Affected flows**: host quits or crashes → client toasts "Host left/disconnected" (UX
  works), single-player continues (works), but the client can **never start a new session**
  (host or join) without restarting the app. The "disable → re-enable" toggle also dead-ends.
  The graceful client-initiated leave (`Joined → Stop` sends PlayerLeave first) is the only
  path that tears down cleanly — so "leave → rejoin" works and "host-leave → rejoin" does
  not, an asymmetry invisible to every milestone acceptance test (which never restarted a
  session after a remote-side end).

**Fix (small, local):** make `Session::Stop()` stop the transport whenever it is running,
independent of `state_`:
```cpp
void Session::Stop() {
    if (transport_.IsRunning()) transport_.Stop();   // always join the socket thread
    if (state_ == SessionState::Idle || state_ == SessionState::Ended) return;
    ...send SessionEnd/PlayerLeave...               // must precede transport_.Stop()
    transport_.Stop();
    state_ = SessionState::Ended;
}
```
(the leave/end messages must still be enqueued *before* the stop; reorder so the send path
runs first, then the unconditional transport teardown). Alternatively, have the coop glue
call `g_session.Stop()` the frame it observes `state() == Ended` on a client. **Add a
selftest**: host+client handshake → simulate remote disconnect (`HandleDisconnect` path) →
assert `Stop()` joins the transport → `StartClient` again succeeds. This is also the gate
for the M5 "longer sessions + rejoin" story.

### MINOR A — Receive-side SceneChange room adoption is not gated on the same-stage flag; a cross-stage move tears the receive slot to (oldStage, newRoom) for up to the 30-frame send window. (cross-module: sender × receiver × the M4.5 same-stage key)

`OnGameMessage` adopts the event's room unconditionally: `slot.state.roomNo =
static_cast<s8>(ev.data & 0xFF)` (`coop.cpp:274-276`), while the *host's* room-table sniff
correctly requires `ev.scene == 1` (`session.cpp:530-537`). A cross-stage mover sends
`scene=0` (`coop.cpp:1087-1095`) but the receiver still overwrites `roomNo` while `stage`
stays the old stage. If the new stage's room number coincides with the receiver's local room,
the puppet's hidden gate (`!sameStage || st.roomNo != LocalRoomNo()`, `coop.cpp:530-533`)
evaluates `sameStage=true && roomNo==local` and **flashes a cross-stage mover visible** for
the 1-2 frames until the first new-stage PlayerState lands (worst case the whole 30-frame
post-change window if the unreliable state drops). **Fix:** gate the adoption on
`ev.scene == 1` exactly like the host sniff — cross-stage moves are then established solely
by PlayerState, which also fixes the room byte in the same write.

### MINOR B — The EnemySnapshot aggro fallback hardcodes player 0; on a client-owned room it reports the wrong target. (latent — the receive side consumes nothing today)

`SendSnapshot` maps the nearest non-puppet player to `aggroId = 0; // the host's own Link`
(`coop_enemy.cpp:620-622`). On a client room owner (selfId = N) the nearest non-puppet is
that owner's own Link — the byte should be `selfId()`. The wire `aggro` is written but has
**no consumer** anywhere on the receive side (grep: only `coop_enemy.cpp:618-625` and the
serializer), so this is dormant today — but M5's enemy-attack-targeting display will read it.
**Fix:** `aggroId = dusk::coop::selfId();` in the fallback; wire a consumer (or drop the
field) before M5.

### MINOR C — Dead code the M4.5 sweep missed: `enemy::onRoomUnload()` has zero callers.

`coop_enemy.cpp:1202` defines `onRoomUnload()`, declared at `coop_enemy.h:147`, **no caller
exists in the tree** (the per-stage reset is handled by `onGameFrame`'s room-change
detection). The M4.5 "drop dead code" commit only swept the `coop.cpp` quartet. **Fix:**
delete, or wire it into the stage teardown if a stage hook is added in M5.

### MINOR D — Per-stage enemy-state reset keys on the room number only; the stage-change case relies on an implicit no-Link gap.

`enemy::onGameFrame` clears the registry when `localRoomNo() != g_lastRoom`
(`coop_enemy.cpp:1156-1163`) — a room number, not a stage. A stage change whose room number
coincides (F_SP103 room 1 → F_SP104 room 1) only resets because the real Link is destroyed
during the transition, so `LocalRoomNo()` returns -1 for ≥1 frame (`coop.cpp:189-193`). That
holds today (multi-frame Link-less transition), but the invariant is implicit and brittle —
if a future change ever keeps the Link alive across a stage transition, the registry would
carry stale `(room<<8)|setID` entries and `RegisterActor` would refuse the new stage's
coincident-id enemies (`coop_enemy.cpp:399`), de-frosting them into local-sim desync.
**Fix:** key the reset on `(LocalStageName(), room)` or on the real-Link pid.

### MINOR E — Protocol/versioning: the `scene` flag is a semantic change inside v6, and the version must be bumped at M5 (M4.5 deepseek MINOR 2, confirmed).

The v6 layout already carried the `scene` byte (never set pre-M4.5), so the *format* is
stable; but an old v6 build in a mixed session sends `scene=0` always, so the new host
ignores its reliable SceneChange and room updates degrade to the unreliable channel for the
session's duration (safe — no bogus ownership entries, only routing latency). The version
gate exists exactly to exclude mixed builds, but both ends being "v6" is what the gate
checks. **Fix:** fold the `scene` semantic into the bump — `kProtocolVersion = 7` at M5
start (M5 adds wire fields anyway: spawn params, horse channel), with the semantic change
documented in the version comment.

### MINOR F — Join rejection and start failure have zero in-game feedback and leave the client stuck until the user toggles `net.enabled`.

`JoinReject` (version mismatch / full / invalid slot) and the 8 s join timeout put the
client in `Rejected` with only a log line (`session.cpp:716-732`, `:397-406`); nothing calls
`Stop()` for `Rejected`, so the session sits inert. `StartHost`/`StartClient` failure sets
`g_startFailed` and retries every 3 s forever (`coop.cpp:858-869`), logging every cycle —
and in the loopback test the most common cause ("port in use", e.g. the other instance still
running) produces exactly this silent 3 s log spam. The settings UI offers no feedback
channel. **Fix:** `NotifyCoop` on `Rejected` with the reason (`rejectReasonName()` already
exists) and on start failure; reset the failed-start state when the relevant `net.*` vars
change; log start-failure causes distinctly (port-busy vs transport-already-running).

### MINOR G — The cliff-edge spawn drop: after 3 create-deadline aborts the puppet is dropped until the remote leaves or a create succeeds.

The puppet spawns +120 units in front of the real Link (`coop.cpp:747-751`); over a cliff the
create spins on the ground check and the 600-frame deadline aborts it
(`coop.cpp:692-703`). After `kCreateDeadlineStrikes = 3` (≈30 s) the limiter sets
`dropped = true` and the spawn is skipped forever (`coop.cpp:735`) — the limiter resets
only on leave (`coop.cpp:805`) or a successful create. A host standing at a cliff edge for
30+ seconds permanently hides the remote player's puppet until they leave/rejoin. This is
the M3-designed "3 strikes" guard, but the strike budget should not be permanent: **Fix:**
reset the limiter when the host's position/room changes materially, or retry the spawn at a
fallback position (e.g. the restart-point-derived anchor from the M4 warp work) instead of
dropping.

### MINOR H — Synthetic-hit collider address stability vs `std::unordered_map` rehash (low probability, cheap to harden).

`SetTgHitSynthetic` stores `&e.synthAt` into the enemy's damage collider and `&e.synthStts`
into the synth (`coop_enemy.cpp:649-682`); `EnemyEntry` is move-only with defaulted moves
(`coop_enemy.cpp:293-296`), so an `unordered_map` rehash (new enemy registered by the
15-frame scan) relocates live entries. Same-frame consumption is the protection: the
injection (pre-actor) → the enemy's own execute (consumes the flag, clears it) → the
draw-phase collision pass (sees no flag). The hazard needs an execute-skip (suspend box /
freeze) between injection and the collision pass *plus* a rehash in the same window, which
would leave the collider pointing at a moved-away synth. **Fix (cheap):** use a node-based
container (`std::list`/`std::map`) or clear the Tg flag in `EraseEntry`/rehash-aware cleanup;
at minimum document the same-frame consumption contract next to the member.

### MINOR I — Doc drift, three instances (all verifiable against the wire):

1. **PlayerState is 2017 B on the wire, not 2001 B.** `PlayerStateWireSize()` computes
   `5 + 16 + 5 + 10 + 1 + 12 + 48 + 1920 = 2017` (`protocol.h`); the docs' field lists omit
   the 16-byte `stage` field: 00-network.md §5 ("~= 2001 bytes"), 02-player-state.md §2
   ("~2.0 KB"), implementation-plan §9 ("2001 B"). §6's bandwidth math (8 × 2001 B × 60 ≈
   0.96 MB/s) is correspondingly ~1% low.
2. **EnemySnapshot is 44 B, not ~28 B.** 00-network.md §5 (provisional schema) and §6
   ("40 enemies × ~28 B ≈ 67 KB/s") predate the v4 layout (`protocol.cpp:173-181` = 44 B;
   at 60 Hz ≈ 105 KB/s). The ~1.0 MB/s worst-case headline still rounds fine.
3. **04-time-weather.md §4 byte counts are envelope-inclusive inconsistently**: TimeSync
   "12" = 8+4 ✓, WeatherChange "10" = 6+4 ✓, but TimeEvent "10" ≠ 8+4 = 12.
   **Fix:** one doc pass against `WireSize`.

### MINOR J — `Announcer::Start` logs a hardcoded `players 1/{}` (M4.5 deepseek MINOR 3, still open).

`discovery.cpp:174` — stale since the glue seeds `SetPlayers` before `Start`
(`coop.cpp:917-919`); read `players_.load()` in the log line. Cosmetic.

### MINOR K — RoomClear still has no direct selftest assertion (M4.5 deepseek MINOR 1, still open).

The M4.5 additions assert `EnemyEvent(Died)` room-scoping only; the receive-side RoomClear
gate (`coop_enemy.cpp:1071-1076`) is the only M4.5 logic with zero coverage. The relay path
is shared with Died so the risk is low, but the two rows (owner B sends RoomClear room=2 →
peer A in room 1 no-ops; A in room 2 lands) are cheap. Also add the explicit **cross-stage**
EnemyEvent row (stage X sender, stage Y receiver with coincident room numbers — the original
MAJOR 2 hazard) to lock the relay's stage-scoping exactly.

### MINOR L — Two near-duplicate sender gates that can drift: static `RemoteInOurRoom` (`coop.cpp:392-411`) vs exported `remoteInRoom` (`coop.cpp:1394-1416`). Both implement the "same-room-or-unknown-opens" rule with slightly different structure; one is the player-sender gate, the other the enemy-snapshot gate. Consolidate into one implementation before M5 adds more senders (waves, horses).

---

## VERIFIED-OK (the big-ticket claims, confirmed with real evidence)

1. **Buildable + selftest-green.** Forced clean game rebuild (2248 files → 33903 exports /
   2053 objects, 2m02s, only the pre-existing ld note); forced clean selftest rebuild →
   `PASS: all checks succeeded`, 318 checks ×2; touched TUs compile warning-free.
2. **Protocol v6 is internally consistent.** `kProtocolVersion = 6` with a full change
   comment (`protocol.h`); all 16 types enumerated; `WireSize` matches every serializer
   field-for-field (hand-checked, above); `DeserializeMessage` rejects wrong-type,
   wrong-size, truncated, and oversized-`jointCount` packets; the determinism sweep runs to
   `MsgType::RoomOwnership` (16).
3. **Transport is sound.** Channel-split SPSC rings (reliable = explicit failure + counters
   surfaced by the session, snapshots = replace-newest); peer-slot generation guard both
   directions; bounded event bursts (32) so inbound can't starve outbound drain; final drain
   before `enet_host_destroy`; oversized/malformed inbound is counted and logged, never
   silently swallowed. The one defect is the *session-level* teardown gap (MAJOR 1), not the
   transport itself.
4. **Room ownership holds under re-derivation.** Sticky first-in (host-default wins on host
   entry), transfer before the roster slot closes in `RemovePlayer`
   (`session.cpp:1034-1053`), `(stage, room)` keying, join-time ownership map,
   `CombatIntent` routed to the room owner (never star-relayed), `EnemySnapshot` +
   `EnemyEvent` RoomScoped with **stage+room** compare in `SendToAllInRoom`
   (`session.cpp:985-992`), `CombatResult` star, receive-side room gates as defense in
   depth. The client-owner case (host not in the room) routes, validates, injects, and
   reports correctly.
5. **Entity-id stability.** `StageEntityId` is a pure `(room<<8)|setID` function — identical
   on every machine and across owners; `DynamicEntityId` is owner-major; the transfer is a
   map transfer, not a renumber. Owner takeover re-sims per the new owner's story
   (documented accepted).
6. **M4.5 fixes are complete in the tree.** Join-warp machinery fully absent (grep);
   `worldStage_` filled from the real Link every frame (`coop.cpp:1195-1201`) and carried in
   JoinAccept/WorldInit; the cross-stage `stageOk` puppet gate keys on the wire stage
   (`coop.cpp:530-533`); the SceneChange `scene` flag is wired sender-side and gated
   host-side; the dead-code quartet is gone. The one missed dead item is `onRoomUnload`
   (MINOR C).
7. **Time/weather sync.** Real 1 Hz TimeSync cadence (`NetClock::AtRate(1)` + `TimeSyncDue`
   + selftest cadence rows); the SeedTargets race guard (reliable WorldInit cannot regress a
   fresher unreliable TimeSync); exact pond segment rule; thunder-bit-edge publish;
   twilight per 04 §5.5; dice suppression gated on `ClientActive()`; stage-change re-assert
   post-`dKy_Create` with the layer-resolution ordering documented; world info in
   JoinAccept/WorldInit.
8. **Combat path.** Client intent capture at `SetAtTgGObjInf` → raw-field `CombatIntent` →
   host routes to the room owner → owner validates (attacker present, no friendly fire,
   target registered, `amIRoomOwner`, range) → synthetic full `dCcD_Sph` Tg-hit injection →
   the enemy's own handler runs its authentic reaction → post-execute `CombatResult` with
   real post-hit HP. The `cc_at_check` slot-0-multiplier limitation is documented
   (m2-design-notes §5). Save-integrity guards verified: create-path (restart-room,
   select-equip, Midna/NPC/horse fan-out, `setStartProcInit`→`procWaitInit`, slot-0 pointer
   save/restore, destructor guards) and damage-path (`setDamagePoint` /
   `setDamagePointNormal` / `setLandDamagePoint` all puppet-gated, `d_a_alink_damage.inc`).
9. **Targeting (D3).** All whitelisted types' player reads route through the scoped context
   (E_YC 6/6, E_AI 3/3 sites; E_HM/E_DF/E_MD/B_TN clean), each with the vanilla read in
   `#else`; `ScopedEnemyTarget` pushes only for owner-simmed registered enemies; the
   fallback outside a scope is slot 0 (vanilla). Per-entry `cullMtx` (M2.5 fix) verified.
10. **Guard hygiene / net-off vanilla.** Every attachment point is `#if TARGET_PC` with the
    vanilla path in `#else` or unchanged; every PC call is session/isPuppet-gated (the
    `d_attention.cpp` ALINK skip is the only ungated PC change and is behavior-neutral
    without a session — one ALINK can exist, and `i_actor == mpPlayer` returns before it).
    Non-PC builds never compile any coop/net code.
11. **Host-leave UX.** Liveness captured pre-`Update` (`coop.cpp:1175-1177`, toast at
    `:1203`), per-reason toast + log, puppets cleared, single-player resumes — verified in
    code; the *rejoin* half is MAJOR 1.
12. **Discovery.** 2 s announce, fixed 44-B datagram, magic + version gating, LAN +
    loopback targets, bounded de-duped list, `SetPlayers` before thread start, clean thread
    teardown, loopback announce→listen selftested.
13. **Shutdown order.** `dusk::config::shutdown()` → `coop::shutdown()` (session Stop) →
    `net::shutdown()` (enet_deinitialize) — correct order, comment fixed (M1 MAJOR M4).

---

## TOP N THINGS FOR THE LIVE PLAYTEST (two instances, loopback)

1. **The MAJOR 1 loop (the one thing the selftest cannot cover):** host quits (graceful
   quit, then a hard `kill`) → client toasts "Host disconnected" → then, *without restarting
   the client*, try to start a new session (host or join) from the Network tab / cvars →
   expect the "already-running transport" failure + 3 s retry spam (known defect). Restart
   the app → rejoin works. Also test the graceful **client-initiated** leave → rejoin in
   the same process (should work).
2. **Cross-stage enemy-kill isolation (MAJOR 2 fix, live):** host in stage A room R, client
   in stage B room R (coincident room numbers, e.g. F_SP103 room 1 vs F_SP104 room 1) → host
   kills a whitelisted enemy → on the client, the same `(room<<8)|setID` enemy must NOT die,
   no wrong drop, no wrong switch grant, client ALLDIE intact.
3. **Stay-put join + meeting:** client in a different save stage joins → no warp, no toast,
   host's puppet hidden → both travel to a shared stage → puppets appear and mirror poses
   exactly; room-change both directions un-sticks both (the mutual-deadlock fix).
4. **Same-room combat + targeting:** both in one room → client's hits land (owner applies),
   HP matches on both screens; kite a whitelisted enemy (E_AI/E_YC) past the client's Link →
   it turns and attacks the nearest player; drops spawn on both machines; doors open
   together (watch the roomClear bit timing vs the owner's native ALLDIE).
5. **Client-owned room:** host and client in different rooms → client enters a room first →
   enemies briefly statue (~1 RTT) then sim natively; client kills → died event reaches only
   the room; client leaves → ownership transfers back (host/next arrival).
6. **Cliff-edge spawn (MINOR G):** host stands at a cliff edge ~30 s with a remote joined →
   check the remote puppet is not permanently dropped.
7. **Time/weather:** same sky, rain arrives on both, wolf-howl fast-forward rate change
   lands within a second on the client, a stage transition re-asserts the clock, Fishing
   Pond (F_SP127) runs the double/triple segment rule, a cutscene freezes both clocks.
8. **Save integrity + net-off baseline:** `USA/Card A/*.gci` mtime unchanged after a full
   co-op session (create/damage guards); then boot with no cvars → vanilla single-player,
   then a full session again — no drift.

## TOP N RECOMMENDATIONS FOR M5 (seams the next developer needs)

1. **Fix MAJOR 1 first** — M5 sessions (waves, PvP, horses) will be longer and rejoins more
   frequent; the "host-leave → rejoin requires restart" defect will bite immediately.
2. **Bump `kProtocolVersion` to 7 at M5 start** and fold in the `scene` semantic change
   (MINOR E). M5 adds wire fields (spawn params, horse entity channel); the bump is the only
   mixed-build protection and the version gate is already stale for `scene`.
3. **Dynamic waves (`EnemyEvent(Spawned)`)** are logged-not-created today: the client cannot
   reproduce owner spawn params. Design the spawn-params contract (procName + params + pos +
   room + the owner-major dynamic id) before adding adapter rows; `registerDynamicEnemy` and
   the id space are ready. The `onRoomUnload` dead code (MINOR C) should be wired or dropped
   in the same pass.
4. **Consume `aggro` (and `semantics`) on the receive side** — enemy-attack targeting of the
   nearest player's puppet needs the byte, and the client-owner fallback bug (MINOR B) must
   be fixed first.
5. **Horse entity channel** — the `Mount`/`Dismount` PlayerEvent ids are reserved; a
   `daHorse_c` puppet follows the ALINK pattern (registry + freeze + pose apply), and the
   `Riding` state flag + `mRideStatus` kind are already on the wire.
6. **PvP** — the `targetPlayerId` field + friendly-fire rejection are ready; the fork's
   `combat` FF-filter machinery (local-coop-poc reference) maps to `SetAtTgGObjInf`
   interception, and the synthetic-collider pattern extends to a `DamagePlayer` intent.
7. **The per-stage enemy reset (MINOR D) and the receive-side SceneChange gate (MINOR A)**
   are cheap hardening that removes two implicit cross-module invariants before the
   wave/horse code multiplies the states they guard.
8. **One doc pass** (MINOR I: 2017-B PlayerState, 44-B EnemySnapshot, 04 byte counts) plus
   the RoomClear/cross-stage selftest rows (MINOR K) — the docs are the M5 developer's map,
   and two of the three drift items are in the "normative" §5/§6 tables.

---

*Review file naming note: this is `review-full-<model>.md` per the capstone convention; the
orchestrator commits it. No source files were modified; the only writes are this review and
gitignored build artifacts.*
