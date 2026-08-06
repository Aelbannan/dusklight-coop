# M4 adversarial review — session polish + room ownership (glm-5.2)

Review range: `0557c5f7b8..HEAD` (M3.5 preamble `a1187d6da5`, `64c9fb01e2`,
`ef95a35ec6`, `b2f307a055`, `20b14cd1d1`; M4 `e700e0945a`, `8431742bc3`,
`e9b0da5b41`, `8b95b741d1`, `51e7f2d1e5`, `e1ebae8847`, `53ad31bc7b`,
`0fa641b36b`, `f1d9de09da`, `c4ca64a356`), branch `net-coop`.

## Evidence gathered (all real)

- **Forced clean rebuild of the game**: `ninja -t clean dusklight` (2248
  files) → `ninja dusklight` → clean compile+link, 33903 exports / 2053
  objects, 1m59s. Re-touched every M4/M3.5 TU (`session.cpp`,
  `discovery.cpp`, `protocol.cpp`, `coop.cpp`, `coop_enemy.cpp`,
  `coop_combat.cpp`, `coop_time.cpp`, `settings.cpp`, `d_a_alink.cpp`,
  `d_kankyo.cpp`): **zero warnings from the M4 TUs** (the only warnings are
  pre-existing vanilla/JSystem: `JKRExpHeap.h:30` multichar,
  `d_kankyo.cpp:3744/3904/4147` tautological s8 compares on untouched vanilla
  lines, `d_a_obj_tks.h:76` via `d_a_alink_demo.inc`, and the harmless
  `ld: ignoring duplicate libraries`). `ninja dusklight` after the touch is
  up-to-date.
- **Forced clean rebuild of the selftest**: `ninja -t clean
  dusk_net_selftest` → `ninja dusk_net_selftest` → clean compile+link.
- **Selftest run**: `./dusk_net_selftest` → **`PASS: all checks succeeded`**,
  **306 checks**, incl. all five M4 suites: `m4: room ownership table`
  (sticky first-in / host-default / transfer / disconnect / stage-key /
  SetOwner), `m4: room-owner routing + same-room snapshot scoping`
  (CombatIntent→room owner B not the host, sticky across arrival, room-1
  intent → host, room-scoped snapshot relay, CombatResult star, takeover on
  leave), `m4: join-warp unlock gate` (decision table + worldStage carry),
  `m4: entity-id stability`, `m4: LAN discovery HostAnnounce` (loopback
  announce→listen, port/players/name payload, counters).
- **Source cross-checks** against vanilla: `d_stage.cpp:1643` (`dStage_playerInit`
  point==-1 branch reads `dComIfGs_getRestartRoomPos/AngleY` directly — never
  touches the PLYR list, so the "failed to find player start point"
  JUT_ASSERT cannot fire), `d_com_inf_game.h:2279` (`dComIfGs_setRestartRoom
  (cXyz, s16, s8)` — the warp call matches), `d_com_inf_game.h:1220`
  (`dComIfGp_setNextStage(stage, s16 point, s8 room, s8 layer, ...)` — the
  warp call passes point -1 and the host layer explicitly).
- **Live-game note**: the TP RVZ used by the M4 loopback tests was removed
  from the repo root (gitignored by `c4ca64a356`), so a fresh full-stack
  playtest could not be re-run in this environment. The stale
  `net-host.log`/`net-client.log` in the tree predate M4 (JoinRequest
  version 5, Aug 6 03:24) and are NOT M4 evidence. The M4 live-loopback
  claims rest on the commit messages (`e1ebae8847`: "Verified live: only
  'F_SP103:1 owner -> 0' exists"; plan §5 M4 status block) — see MAJOR 1 for
  the one flow those live runs did not exercise.

---

## VERDICT

**M4 is a solid, selftest-green milestone and meets most of its acceptance
criteria, with one MAJOR functional gap in the join-warp (D6) and one MAJOR
protocol-scoping hole in enemy events.**

- **Room ownership — MEETS.** Sticky first-in table, host-defaults-own-
  its-room, transfer on leave/disconnect only, `(stage, room)` keying,
  `RoomOwnership` v6 wire object + join-time map broadcast, CombatIntent
  routing to the room owner (host relays to the owner peer; the owner
  validates via `amIRoomOwner(enemy room)`), EnemySnapshot owner→room
  (sender gate + RoomScoped relay + receive-side decode), entity-id
  stability (pure stage keys + owner-major dynamic ids). The selftest drives
  the full routing matrix with a non-host owner.
- **Join-warp (D6) — PARTIAL.** The unlock-gate decision, the refuse/safe-
  anchor path, the `worldStage_` fill (the M1 inert-TODO is now live), and
  the `dStage_playerInit` assert interaction are all real and verified. But
  the Warp path never fires for its designed purpose (a mid-game join into
  the host's stage) and, when it does fire, anchors on a stale position
  (MAJOR 1): at join time the host's stage can never be in the client's
  session-reached set, so a fresh join is always `NoWarp` (same stage) or
  `RefuseAnchor` (unreached); `Warp` only fires later, as an undocumented
  teleport-back "leash" the moment the client steps out of the shared stage,
  and the anchor position is the host's last-transmitted PlayerState — the
  host's sender gate blocks cross-stage states, so the live position the warp
  waits for is unobtainable by construction.
- **Host-leave UX (D8) — MEETS.** Live→dead transition captured pre-Update,
  toast per `SessionEndReason` (HostLeft vs ConnectionLost), puppets
  despawned by the existing teardown, sender trackers + receive slots
  cleared; single-player resumes.
- **LAN discovery — MEETS.** 2 s broadcast announce (LAN + loopback),
  fixed-size v6-gated datagram, bounded de-duped listener list, coexists
  with manual IP join, clean thread teardown, no storms.
- **M3.5 preamble — all five items landed and verified** (wolf puppet
  callback gate, seed-decision table test, d_kankyo comment, 04 §5.11 +
  TESTING.md docs, thunder-bit publish edge).
- **Regression / hygiene — MEETS.** Clean forced rebuild, 306-check
  selftest green, net-off path still gated (`net.enabled=false` → session
  never starts, all attachment points no-op), no M5 features (dynamic
  `Spawned` events are logged-not-created, friendly fire rejected, no horse
  channel), chores commits harmless.

## RANKED FINDINGS

### BLOCKER
None. (No build/link/selftest failure; nothing prevents booting the game or
running the co-located playtest.)

### MAJOR

**MAJOR 1 — The D6 join-warp Warp path never executes for its designed
purpose (mid-game join into the host's stage) and, when it fires, anchors
on a stale position with no notice (the "leash").**

**Evidence, part 1 — Warp is unreachable at join.** `DriveJoinWarp` seeds
`g_reachedStages` only from the current session's stage transitions plus the
save's stage (`coop.cpp:899-920`); `DecideJoinWarp` returns `Warp` only when
the host's stage ∈ reached ∧ `myStage != target` (`coop_join_logic.h:77-86`,
`coop.cpp:927-952`). A fresh join boots the client into its save stage, so
at join: target == save stage → `NoWarp` (already there), or target ≠ save
stage → `RefuseAnchor` (the host's stage is not in the session-reached set —
the client cannot have entered it this session before joining). The mid-game
join-into-host-stage flow D6 exists for (00-network.md §4 "Join mid-game:
client warps to the host's stage/room") therefore always refuses or no-ops;
"a mid-game joiner who saved in the host's stage" (the doc's own example in
`coop_join_logic.h`) evaluates `NoWarp` because it is already there.

**Evidence, part 2 — when Warp does fire it is a stale-anchor leash.** The
only way the host's stage enters the reached set is the client having been
in it this session, so `Warp` fires only *after* the client left the shared
stage — and then it fires every frame the client is outside it. The anchor
is `g_receive[0].state` (`coop.cpp:961-969`, `PerformJoinWarp` at
`coop.cpp:974-985`), which is the host's **last-transmitted** PlayerState:
the host's send is gated by `RemoteInOurRoom` (`coop.cpp:1203`;
`coop.cpp:390-411`), and for a cross-stage client the gate is closed
(`sameStage=false`, `myRoomAtLastRecv == myRoom`), so no fresh state can
arrive. The client is therefore silently teleported back to the host's
departure-time position — not its live one (the code's stated intent,"so the
client lands beside the host") — with no toast, contradicting the documented
cross-stage coexistence (00-network.md §2: "Clients in different scenes
simply don't render each other"; the RefuseAnchor comment at
`coop.cpp:932-935`). The 600-frame timeout (`coop.cpp:962-966`) merely
clears `g_warpPending` and re-arms (`coop.cpp:945-952`) — defensive dead
code, since `g_receive[0].hasState` is sticky-true once any host state was
ever received.

The live loopback test exercised only the same-stage case (`NoWarp` — both
instances used the same save); the cross-stage `Warp` case was never
verified live.

**Fix (small):** seed the reached set from the client's story/save flags
(TP has none — the honest alternative is to accept a warp on explicit user
confirmation), or make the host stream to a cross-stage peer at a low rate
so the anchor is live, or latch the warp decision to fire once with the
`worldStage_` room + a stage start point instead of the host's position and
add a notice. Whatever the choice, add a live-loopback acceptance step:
host in F_SP108, client in F_SP103 with F_SP108 reached → the client joins
and reaches the host (or is told exactly why not), and a client stepping
out of the shared stage is not silently teleported back.

**MAJOR 2 — `EnemyEvent(Died)` and `EnemyEvent(RoomClear)` have no
stage/room scoping on the receive side; a died event from a different stage
with a coincident `(roomNo, setID)` mis-kills a client's local enemy,
spawns a wrong drop, grants a wrong room switch, and corrupts ALLDIE.**

**Evidence:** `coop_enemy.cpp:1054-1059` — `EnemyEvent` handling is
unconditional: `OnEnemyDied(ev.enemyId, ...)` looks up `g_entries` by id
(`coop_enemy.cpp:775-791`), and `RoomClear` sets `g_roomClear[enemyId]`
for any `enemyId < 256`. The M4 receive-side scoping work (53ad31bc7b)
covered **only** `EnemySnapshot` (`coop_enemy.cpp:1036-1043`, room-decode
check); `EnemyEvent` is relayed `RelayPolicy::Star` to every peer
(`session.cpp:56-57`), which is a documented design choice ("receivers
no-op for rooms/ids they do not have"). That no-op claim holds only for
same-stage different-room (the id encodes the room) — it **fails across
stages**: stage-placed ids are `(roomNo << 8) | setID` (`coop_entity_logic.h`),
and a client in stage B room R with a whitelisted enemy of setID S has the
identical id `(R<<8)|S` as stage A's room-R setID-S enemy. A died event for
stage A's enemy then deletes the stage-B actor (after the 18-frame death
beat), spawns stage A's drop table in stage B, calls
`GrantLocalSwitch(flagMask, e.roomNo)` (`coop_enemy.cpp:757-766`) — a
**save-flag write in the client's own story** — and empties the client's
room-clear scan early. Cross-stage coexistence is a first-class M4 state:
the join-warp refusal leaves both players in different stages by design
(plan §5 M4, "the players do not see each other until they are in a shared
stage"), and clients may wander stages freely after joining.

**Fix:** route `EnemyEvent` room-scoped like `EnemySnapshot`
(`PolicyFor(EnemyEvent) = RoomScoped` in `session.cpp:50-57`) and/or add a
receive-side stage/room check (the stage is not on the wire — the simplest
sound fix is the relay-scope change; the sender's room is the room it
owns). Same for `RoomClear` (its `enemyId` field is a bare room number
with no stage).

### MINOR

**MINOR 1 — Cross-stage `SceneChange` creates a transient bogus
`(oldStage, newRoom)` ownership entry; intents in the window route to the
wrong owner and drop.** `session.cpp:503-518` — the reliable SceneChange
handler keys the table with the player's *last-known* stage (`playerRoom_`
was seeded by the previous stage's PlayerState) plus the new room number.
On a cross-stage move with the SceneChange (channel 0) arriving before the
first new-stage PlayerState (channel 1, no cross-channel ordering), the
host records the mover in `(oldStage, newRoom)`; if that room was
ownerless the mover transiently "owns" it, a spurious `RoomOwnership`
broadcast fires, and a `CombatIntent` sent right after (same channel 0,
FIFO) routes to the wrong room's owner and is dropped. Self-corrects on
the first new-stage PlayerState (1-2 frames on LAN); same-stage moves are
safe because SceneChange precedes the intent on the reliable channel.
**Fix (future):** carry the stage in the `PlayerEventMsg.scene` field
(allocated on the wire, currently always 0) or gate the sniff until the
stage is confirmed.

**MINOR 2 — Dead code / dead state left by the M4 refactor:** `setWorldStage`
(`coop.h:113`, `coop.cpp:1382`) has no callers (the host fills
`g_session.setWorldStage` directly at `coop.cpp:1297`); `sessionEndReason()`
(`coop.h:133`, `coop.cpp:1426`) has no callers (`NoticeSessionEnd` reads
`g_session.endReason()` directly); `hostWorldStageKnown()` (`coop.h:135`,
`coop.cpp:1430`) has no callers; `g_lastDiscoveryLogMs` (`coop.cpp:183`,
reset at 1341) is written but never read after e1ebae8847 moved the
per-session logging into the listener. All four should be deleted or wired.

**MINOR 3 — Commit hygiene:** `tools/run-coop.sh` (an M4 artifact) and seven
prior-milestone review files were swept into the M3.5 thunder-bit commit
`20b14cd1d1` together with the stray `net-host.log`/`net-client.log` that
`f1d9de09da` later removed. No content issue (the log drop + gitignore are
correct and harmless), but the M4-range history mixes unrelated files.

**MINOR 4 — Doc drift (small):** `00-network.md` §10 says new sessions are
"logged (`coop: discovered session ...`)"; the glue log was removed in
e1ebae8847 and the actual line is `discovery: found session ...` emitted by
`discovery.cpp`. One sentence in §10 and the §4 HostAnnounce row should say
`discovery: found session`.

**MINOR 5 — The selftest determinism sweep stops at `WeatherChange` (15)
and does not include `RoomOwnership` (16).** `selftest_main.cpp:443` —
`for (u16 t = ...; t <= MsgType::WeatherChange; ++t)`. The round-trip check
does cover v6 and the zero-init union + explicit reserved writes make a
determinism failure implausible, but the loop bound should be
`MsgType::RoomOwnership` to match the "all 16 types" claim at line 386.

**MINOR 6 — First announce datagrams carry `players=0`.** `discovery.cpp`
— `players_` defaults to 0 and the coop glue calls `SetPlayers` on the
frame *after* `Announcer::Start` (`coop.cpp:1030`/`1038`), so up to ~2 s of
announces advertise 0 players. Cosmetic; initialize the atomic from the
`BuildAnnounce` value or call `SetPlayers` before `Start`.

**MINOR 7 — A client's `OwnsRoom` lags the host's assignment by one RTT.**
The client's ownership view is message-only (`session.cpp:414-427`
`OnRoomOwnership` → `SetOwner`); entering a room as first-in (ownerless →
client becomes owner) the client freezes its own room's enemies
(`coop_enemy.cpp:351-358` `OwnsRoom` false → `puppetExecute` skips AI) for
the ~1 RTT until the host's broadcast lands, so room-entry enemies briefly
statue before snapping to native AI. Transient by design (host-authoritative
table); acceptable, worth a note.

**MINOR 8 — `static bool sSaveStageSeeded = g_saveStageSeeded;`
(`coop.cpp:910`) defeats the `shutdown()` reset.** The function-local
static initializer runs once per process, so a second session in the same
process never re-seeds `dMeter2Info_getSaveStageName` into the fresh
`g_reachedStages`. Self-corrects (the current stage is added once the play
scene loads, and the save stage is where the client boots), so impact is
nil in practice; the global `g_saveStageSeeded` is effectively dead state.
Use the global directly or drop the reset.

**MINOR 9 — Two clients on one machine: the second listener's `bind(0.0.0.0:44771)`
can fail on macOS** (`discovery.cpp` `Listener::ThreadMain`, "another
listener?" warn path), so the second client misses loopback announces
(real LAN broadcasts are still delivered to the first binder; unicast
127.0.0.1 announces go to the last binder). Edge case of the loopback test
setup only; manual `net.joinHost` covers it.

---

## VERIFIED-OK

1. **Protocol v6**: `kProtocolVersion = 6` with a full version-change
   comment; `RoomOwnershipMsg` layout (stage[16]+room s8+owner u8+reserved[2]
   = 20 B) matches `WireSize` (protocol.cpp:194-197), serializer and
   deserializer field-for-field, envelope type-range extended to
   `RoomOwnership` (protocol.cpp:291-294), `ChannelFor` reliable
   (default branch). 00-network.md §5 row + §2/§4 updates landed.
2. **Room-ownership table** (`session.cpp:96-233`): sticky
   (`OnPlayerEnter` never displaces an owner except the host-default rule;
   selftest 1 proves no ping-pong), transfer on leave/disconnect
   (`OnPlayerLeave`/`OnPlayerDisconnect` → next arrival or ownerless),
   `(stage, room)` keying (selftest 5), owner broadcast on change +
   full map to each joiner (`SendOwnershipMap` after WorldInit), client
   `SetOwner` view. `RemovePlayer` transfers ownership before the roster
   slot closes (session.cpp:1016-1032).
3. **Routing**: `RouteCombatIntent` (session.cpp:824-856) routes by the
   attacker's last-known room to the owner peer (or consumes locally when
   ownerless/owner-0); `CombatIntent` is never star-relayed; the owner
   validates (attacker present, no friendly fire, target in its own
   registry, in range, `amIRoomOwner` of the enemy's room — coop_combat.cpp
   :119-160); `EnemySnapshot` is RoomScoped both for the host's own fan-out
   (`SendGameMessage`) and the relay of a client owner's copies
   (`ForwardGameMessage`), with a receive-side room decode as defense in
   depth. Selftest proves: intent to non-host owner B, not echoed to A, not
   consumed by the host; room-1 intent reaches the host; room-2 snapshot not
   relayed to a room-1 peer; CombatResult star-reaches everyone except the
   origin.
4. **Same-room scoping**: sender gate `remoteInRoom` (coop.cpp:1396-1420)
   scopes by the remote's wire stage+room (not "any present player",
   M2 MINOR-6 resolved) with the unknown-room-opens rule; relay uses the
   sender's `(stage, room)`; the host's own snapshots use `playerRoom_[0]`.
5. **Entity-id stability**: `StageEntityId` is a pure function of
   `(roomNo, setID)` — identical on every machine and across owners (map
   transfer, not renumber); `DynamicEntityId` is owner-major (bit15 +
   owner in bits 12-14 + 12-bit counter) so two owners' spaces never
   collide; the old `(0x8000..)` counter semantics were replaced
   consistently; selftest asserts the invariants.
6. **Owner takeover**: transfer path end-to-end selftested (B leaves → A
   owns room 2); re-sim per the new owner's story is the documented
   accepted behavior (plan §5 M4); the `roomClear` bit (`hostRoomCleared`
   owner-gated, once-per-room latch) keeps non-owner ALLDIE scans honest
   (`clientRoomClearGated`), wired at `d_a_alldie.cpp:40-41/60`.
7. **Join-warp**: `worldStage_` filled from the real Link every frame
   (coop.cpp:1291-1298 — the M1 TODO is live) and carried in
   JoinAccept/WorldInit (selftest asserts stage+room arrival); the
   `dStage_playerInit` point==-1 branch (`d_stage.cpp:1665-1667`) reads
   restart pos/angle directly — the PLYR-list assert is unreachable for
   this warp; the layer is passed explicitly and the client seeded the
   host's time from WorldInit first (04 §5.10 updated to match); the
   RefuseAnchor path notifies once and stays at the safe anchor (this is
   the path a fresh join actually takes — see MAJOR 1); the
   transient-room-keys fix (empty-stage guard in `UpdatePlayerRoom` +
   SceneChange-sniff stage gate) is sound.
8. **Host-leave UX**: liveness captured before `Session::Update` drains the
   inbox (coop.cpp:1274-1276), toast + log per reason, `PumpSessionAndSpawns`
   clears puppets/receive slots/sender trackers, discovery threads stopped
   in `shutdown()` (coop.cpp:1324-1331). Settings → Network tab binds all
   five `net.*` vars with `config::save()` and lists discovered sessions;
   client `net.hostPort` semantics (host's port) documented in the UI, the
   config header, and run-coop.sh.
9. **LAN discovery**: 2 s interval (no storms), fixed 44-B datagram,
   magic + version gating, LAN broadcast + loopback targets, bounded
   de-duped list, thread teardown joins promptly (250 ms sleep slices),
   coexists with manual IP join; loopback announce→listen selftested.
10. **M3.5 preamble**: wolf `wolfModelCallBack` isPuppet gate
    (d_a_alink.cpp:2535-2549, additive `#if TARGET_PC`, return-1 semantics
    matching the human gate — wolf puppets noted for playtest);
    `SeedTargetsDecision` table + selftest rows; d_kankyo.cpp:8374-8377
    comment now "after the start-room LAYER is resolved"; 04 §5.11 exact
    pond rule + TESTING.md status; `WeatherPublishDue` includes
    `thunder != g_lastThunder` (coop_time.cpp:317-323) with a regression
    guard proving the old gate missed the edge.
11. **Regression / conventions**: clean forced game rebuild + link; selftest
    306/306 on a forced rebuild; M4 TUs compile warning-free; net-off boot
    stays gated (EnsureSession stops on `net.enabled=false`, all attachment
    points check `sessionActive`/`isPuppet`, `fopAc_Execute` gate at
    f_op_actor.cpp:296-304); no M5 features; `TARGET_PC` hygiene consistent;
    `net-*.log`/`*.rvz`/`.pi/` gitignored.

## MUST-FIX (for the final full-stack playtest)

1. **MAJOR 1** (join-warp) — a mid-game join into a reached-but-different
   host stage never warps (always `RefuseAnchor`/`NoWarp`), and the only
   Warp path is a silent stale-anchor teleport-back leash when the client
   leaves the shared stage. Pick a behavior (warp on user confirmation with
   a live or stage-start anchor, or a documented stay-put policy), fix it,
   and add the cross-stage live-loopback acceptance step.
2. **MAJOR 2** (unscoped EnemyEvent) — one relay-policy change
   (`RoomScoped` for `EnemyEvent`) before the multi-stage playtest; the
   cross-stage refusal state is a designed flow, and a wrong switch grant
   touches a client's save.

Both fixes are small and local; neither blocks the co-located (same-room /
same-stage) playtest path, which is fully exercised by the selftest and the
documented live loopback.
