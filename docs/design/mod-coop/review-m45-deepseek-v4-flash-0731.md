# M4.5 adversarial review — final fix pass (deepseek-v4-flash-0731)

Review range: `0fa641b36b..HEAD` (M4.5: `ea68f73c96` join-warp removal,
`9904fb8403` EnemyEvent room scoping, `8fd89c2bf7` SceneChange stage key,
`4ce3365b68` dead code, `e942738d79` docs, `99d50fd95d` complete; plus the
in-range chores `f1d9de09da` log drop + `c4ca64a356` gitignore), branch
`net-coop`. Aurora at upstream `6c4c27f9` — not a regression.

## Evidence gathered (all real)

- **Forced full clean rebuild of the game**: `ninja -t clean dusklight` →
  `ninja dusklight` → clean compile+link, **33903 exports / 2053 objects**,
  4m40s. Only the pre-existing `ld: ignoring duplicate libraries` note and
  the known vanilla/JSystem warnings; a targeted re-touch rebuild of the six
  M4.5 TUs (`coop.cpp`, `coop_enemy.cpp`, `session.cpp`, `protocol.cpp`,
  `discovery.cpp`, `selftest_main.cpp` + the touched headers) produced
  **zero warnings** (grep filtered only the known pre-existing
  JKRExpHeap/tautological-s8/d_a_obj_tks/duplicate-library lines).
- **Selftest**: `ninja dusk_net_selftest` clean, then
  `./dusk_net_selftest` → **`PASS: all checks succeeded`, 318 checks
  (was 306)**, **three consecutive runs** green (the discovery suite's
  loopback timing is stable). All 18 suites ran, incl. the rewritten
  `m4: worldStage carry (stay-put join policy, D6 revised)`, the room-scoped
  EnemyEvent rows (`room-2 died event NOT relayed to the room-1 peer A` /
  `relayed to the room-2 peer A`), and the cross-stage SceneChange rows
  (`cross-stage SceneChange did not move A in the room table` / `no bogus
  (F_SP108, 7) owner entry`).
- **Grep sweep**: `DecideJoinWarp` / `PerformJoinWarp` / `DriveJoinWarp` /
  `SafeStageSet` / `sSaveStageSeeded` / `g_saveStageSeeded` /
  `g_reachedStages` / `g_warpPending` / `g_warpRefusedNotified` /
  `g_lastDiscoveryLogMs` / `sessionEndReason` / `hostWorldStageKnown` →
  **zero hits in src/ or include/** (only docs/commit-history references in
  review-m4-glm-5.2.md and the plan's D6 row). `coop_join_logic.h` deleted
  in `99d50fd95d`; no include of it remains anywhere.
- **Live loopback not re-runnable here** (no RVZ in the tree; same
  limitation as the M4 review) — the stay-put join flow rests on the
  selftest + code inspection; the final playtest must re-verify live.

## VERDICT

**M4.5 resolves both MAJORs and every MINOR it claims. HEAD is buildable,
selftest-green, and regression-clean; the branch is READY for the final
full-stack playtest.**

- **MAJOR 1 (join-warp) — RESOLVED (removal per user decision).** The entire
  warp machinery is gone from the source: no decision table, no reached-set
  seeding, no 600-frame timeout, no leash, no `dComIfGp_setNextStage` /
  `dComIfGs_setRestartRoom` / `dStage_changeScene` anywhere in the coop
  layer. What the policy requires is **kept and live**: the host fills
  `worldStage_` from the real Link every frame (`coop.cpp:1194-1200`), the
  stage rides JoinAccept (`session.cpp:653`) and WorldInit (`session.cpp:664`)
  to the client (selftested: client reads `worldStage().stage == "F_SP103"`
  room 4), and the cross-stage `stageOk` puppet gate keys on the remote's
  **REAL wire stage** (`coop.cpp:530-533`: `hidden = !sameStage ||
  roomNo != LocalRoomNo()`, consumed by `puppetDrawHidden` at
  `d_a_alink.cpp:19678`) — a client in its own save stage still hides the
  host's puppet until both travel to a shared stage. The client join path is
  now exactly "boot into my save stage and stay"; no code path moves it.
- **MAJOR 2 (EnemyEvent scoping) — RESOLVED.** `PolicyFor(EnemyEvent) =
  RoomScoped` (`session.cpp:59-70`), which covers BOTH `Died` and
  `RoomClear` through the shared host fan-out (`SendGameMessage` →
  `SendToAllInRoom`, stage+room keyed at `session.cpp:987-992`) and the
  inbound relay (`ForwardGameMessage` uses the origin's last-known
  `(stage, room)` at `session.cpp:816-829`). Cross-stage peers with a
  coincident room number are excluded by the stage compare in
  `SendToAllInRoom`, so the `(roomNo<<8)|setID` mis-kill / wrong-drop /
  wrong-switch / ALLDIE corruption paths are closed at the relay. Defense in
  depth on receive (`coop_enemy.cpp:1060-1074`) drops stage-placed Died
  events whose decoded room differs from the local room and RoomClear for
  any room but the local one. The selftest proves a room-2 died event is
  not delivered to a room-1 peer and is returned to it once both share the
  room. (RoomClear itself has no direct assertion — see MINOR 1.)
- **MINORs — RESOLVED.** MINOR 1 (SceneChange stage key): wire `scene` flag
  (already-allocated field, no size change), sender marks same-stage vs
  cross-stage, host sniff gated on `scene==1` + the retained empty-stage
  guard; selftest rows prove no bogus entry and correct same/cross behavior.
  MINOR 2 (dead code): all four removed, grep-clean, no new dead code.
  MINOR 4 (discovery log doc): §10 + §4 HostAnnounce rows say
  `discovery: found session ...` matching `discovery.cpp:312`. MINOR 5
  (determinism bound): sweep now ends at `MsgType::RoomOwnership`
  (`selftest_main.cpp:445-447`) — all 16 types. MINOR 6 (players=0):
  `SetPlayers` runs before `Start` (`coop.cpp:917-919`), thread-creation
  happens-before guarantees the first datagram; selftest asserts
  `players == 2`. MINOR 8: gone with the warp machinery.
- **Regression — CLEAN.** Forced full rebuild + link green; selftest 318/318
  x3; net-off boot untouched (`EnsureSession` stops on
  `net.enabled=false`, `coop.cpp:821-830`; the M4.5 diff touches no
  attachment-point file — `d_a_alink.cpp`, `d_kankyo.cpp`, `settings.cpp`,
  `d_a_alldie.cpp` are all outside the range); M4 room ownership /
  discovery / host-leave UX, M3 time-weather, M2 enemy authority, M1 player
  replication code paths unmodified; no M5 features (dynamic `Spawned`
  events still logged-not-created); the two in-range chores are gitignore /
  log-drop only.

## RANKED FINDINGS

### BLOCKER
None. (Build, link, and selftest all pass; nothing prevents booting the
game or running the co-located playtest.)

### MAJOR
None.

### MINOR

**MINOR 1 — The RoomClear half of MAJOR 2 has no direct selftest
assertion; the new receive-side gate is the only M4.5 logic with zero
coverage.** `coop_enemy.cpp:1068-1074` (the `RoomClear` branch:
`ev.enemyId < 256 && (s8)ev.enemyId == localRoomNo()`) is new code; the M4.5
selftest additions cover `Died` only (`selftest_main.cpp:2206-2265` — the
`EnemyEvent(died)` rows). The relay policy is shared per-type (both ride the
RoomScoped branch of `PolicyFor`/`SendToAllInRoom`), so "room-clear reaches
only the sender's room" is proven transitively by the Died rows — but a
cross-room RoomClear being no-op'd on receive is not asserted anywhere, and
the commit message claims it. **Fix (small):** one pair of rows in
`RunM4RoomRoutingCheck` — owner B sends `EnemyEvent(RoomClear, enemyId=2)`
while A is in room 1: assert A's handler sees the event (relay) but the
gate no-ops it (e.g. expose a `roomClearFor(room)` accessor for the test or
assert via the client's gated-scan helper), then repeat with A in room 2 and
assert it lands.

**MINOR 2 — The SceneChange `scene` flag is a wire-semantic change inside
v6, not versioned.** `PlayerEventMsg.scene` was "allocated on the wire,
always 0" and the M4 review itself suggested using it, but an old v6 build
(e.g. `0fa641b36b`) joining a new host always sends `scene=0`, so the new
host's sniff (`session.cpp:530-537`, `ev.scene == 1`) now ignores that
client's reliable SceneChange entirely — room updates degrade to the
unreliable PlayerState channel (the exact 1-2-frame transient MINOR 1
removed, minus the bogus-entry corruption, which is gone). Direction is
safe (no entry is created, only a brief routing delay) and only occurs in a
mixed-build LAN session the version gate is meant to exclude; the reverse
mix (new client, old host) behaves exactly as M4 did. **Fix:** bump
`kProtocolVersion` to 7 with a comment, or document that mixed-build v6
sessions lose the same-stage sniff authority. (Both ends of a session are
the same build in practice, so impact is nil today.)

**MINOR 3 — `Announcer::Start` logs a hardcoded `players 1/{}`
(`discovery.cpp:174`), now stale after the MINOR 6 fix.** The glue seeds
`SetPlayers` *before* `Start`, so the log could print the real count (the
datagrams already do — `discovery.cpp:206`). Cosmetic only, same class as
the MINOR 4 doc drift M4.5 just cleaned. **Fix:** read
`players_.load()` in the `Start` log line.

## VERIFIED-OK

1. **Join-warp removal is complete and consistent** (grep sweep above);
   `coop_join_logic.h` deleted; the coop layer contains no scene-change /
   restart-room call of any kind (the only `dComIfGp_setNextStage` /
   `dComIfGs_setRestartRoom` users in the tree are the vanilla pause-map
   warp `src/dusk/ui/warp.cpp` and the dev-only `ImGuiStateShare` — both
   pre-existing and unrelated).
2. **Stay-put join path is the only join path**: client-side session start
   → join handshake → `worldStage` carry → own-stage boot; the engine never
   receives a net-driven stage change. The `worldStage` carry selftest
   (`RunM4WorldStageCheck`) proves host publish → client receipt
   (stage + room).
3. **Cross-stage `stageOk` gate intact and live**: wire-stage compare at
   `coop.cpp:530-533` → `puppetDrawHidden` (`coop.cpp:1160`) → draw gate
   `d_a_alink.cpp:19678`; a different-stage remote's puppet is hidden (stays
   hidden until both players share a stage — the stay-put policy's
   visibility contract).
4. **EnemyEvent relay room-scoping**: `PolicyFor(EnemyEvent) = RoomScoped`
   (`session.cpp:63-70`); host fan-out `SendToAllInRoom` compares **stage
   AND room** (`session.cpp:987-992`), so a cross-stage same-room peer is
   excluded; the unknown-room rule for the host's own fan-out falls back to
   `SendToAll` only when the host has no room yet (`room.room < 0`), and the
   inbound relay drops when the origin's room is unknown
   (`session.cpp:824-826`); both match the sender-gate semantics.
5. **Receive-side defense in depth**: stage-placed Died events
   (`< kDynamicIdBase`) gate on the decoded room == `localRoomNo()`
   (`coop_enemy.cpp:1061-1066`), dynamic ids pass (v1 never produces them —
   `Spawned` is logged-not-created), RoomClear gates on the bare room
   (`coop_enemy.cpp:1068-1074`), `g_roomClear[256]` indexing stays in
   bounds (gate requires `< 256`; `StageEntityId` never emits negative-room
   ids).
6. **MAJOR-2 selftest rows**: room-2 `Died` NOT relayed to the room-1 peer
   A, host still consumes it, not echoed back to owner B; relayed to A once
   both peers share room 2 (`selftest_main.cpp:2206-2265`); the M2 relay
   suite row re-documented as the unknown-room open-gate case
   (`selftest_main.cpp:1047-1058`).
7. **SceneChange stage key**: `scene` flag documented in `protocol.h:
   351-356`, serialized at `protocol.cpp:228/322` (field was already on the
   wire — zero size/version impact); sender computes
   `scene = stageChanged ? 0 : 1` (`coop.cpp:1089-1097`); host sniff requires
   `scene == 1` + non-empty last-known stage (`session.cpp:530-537`);
   selftest rows: `scene=0` cross-stage move does not move the table and
   creates no `(F_SP108, 7)` entry; the new-stage PlayerState establishes
   `(F_SP104, 7)` with A owning it; `scene=1` same-stage moves remain the
   room-change authority.
8. **Dead-code removal**: `coop::setWorldStage`, `sessionEndReason()`,
   `hostWorldStageKnown()`, `g_lastDiscoveryLogMs` — all gone (grep-clean);
   the `d_meter2_info.h` include dropped with the seeding code; no new dead
   code introduced in the M4.5 TUs (the only `setWorldStage` remaining is
   `Session::setWorldStage`, used by `coop.cpp:1199` and the selftest).
9. **MINORs 4-8**: discovery log-line docs match `discovery.cpp:312`
   (`discovery: found session ...`); determinism sweep runs all 16 types to
   `MsgType::RoomOwnership` and the protocol suite's header claim matches;
   announce player count seeded before the thread starts and the selftest
   asserts `players == 2` / `maxPlayers == 4`; `sSaveStageSeeded` gone with
   the feature; `TESTING.md` + plan §5/D6/risk-8 + `00-network.md`
   §2/§4/§5/§10/§11 all updated to the stay-put policy (no doc claims a
   warp or teleport-back anywhere).
10. **Regression / hygiene**: forced full clean rebuild green (33903
    exports / 2053 objects, 4m40s); selftest 318/318 on three consecutive
    runs; net-off boot gated as before; the M4.5 range touches no M1/M2/M3/M4
    game-code paths other than the intended ones; no M5 features; the two
    in-range chores (`f1d9de09da` drop stale runtime logs + gitignore,
    `c4ca64a356` gitignore disc image + local `.pi`) are harmless and the
    stale `net-host.log`/`net-client.log` on disk are gitignored leftovers
    (Aug 6 03:24, pre-M4) — not evidence.

## MUST-FIX (for the final full-stack playtest)

1. **Re-run the live loopback end-to-end with the RVZ** (the one thing this
   environment cannot do — same as the M4 review): host boots into a stage,
   a client with a DIFFERENT save stage joins → client stays put, host's
   puppet hidden, no teleport; both travel to a shared stage → puppets
   appear; plus one cross-stage enemy-kill sanity pass (host kills an enemy
   in its room while the client is elsewhere: nothing on the client's
   screen dies, no wrong switch is granted, ALLDIE stays intact). The
   selftest covers the network matrix; the live check covers the engine
   integration (Link-stage fill, `fopAcM_GetRoomNo` values, draw gate).
2. (Optional, small) Add the RoomClear selftest rows from MINOR 1.

Both MAJORs and all claimed MINORs are resolved; nothing blocks the
playtest.
