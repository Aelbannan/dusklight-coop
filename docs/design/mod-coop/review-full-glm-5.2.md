# Full-implementation adversarial review — networked co-op (net-coop, HEAD)

Reviewer: glm-5.2 (capstone holistic pass)
Range: the whole branch — `95608438c1..HEAD` (M0 → M4.5, 50 net commits), branch
`net-coop`. This is the **sum-of-the-parts** pass on top of the ten per-milestone
reviews (M0/M0.5/M1/M1.5/M2/M2.5/M3/M3.5/M4/M4.5 — all approved). Every individual
milestone is assumed correct; this review attacks the WHOLE: cross-module,
end-to-end, lifecycle boundaries, doc-vs-code drift across the full trail.

## Evidence gathered (all real)

- **Forced clean rebuild**: `ninja -C build/macos-default-relwithdebinfo -t clean`
  (2267 files) → `ninja ... dusklight dusk_net_selftest` → clean compile+link of
  the full game (`Dusklight.app/Contents/MacOS/Dusklight`, 48.6 MB) +
  `dusklight-stub` + `dusk_net_selftest`. **Zero warnings from any net/coop TU.**
  The only warning in the entire rebuild is the pre-existing vanilla
  `JKRExpHeap.h:30` multi-character constant (`'HM'`), a JSystem header pulled in
  transitively by `src/dusk/ui/settings.cpp`, untouched by the net work.
- **Selftest**: `./dusk_net_selftest` → **`PASS: all checks succeeded`**,
  **318 `ok` checks**, exit 0. All 18 suites green (protocol round-trips +
  determinism sweep over all 16 types, handshake, relay policies, ring overflow,
  1 Hz clock, M3.5 decisions, generation guard, duplicate-join, JoinAccept
  validation, oversized metric, M4 ownership table/routing, worldStage carry,
  entity-id stability, LAN discovery). The "reliable outbox ring full" log lines
  are the deliberate-flood overflow test, not a failure.
- **Source cross-checks**: read every net/coop TU
  (`transport.cpp/h`, `session.cpp/h`, `protocol.cpp/h`, `discovery.cpp/h`,
  `coop.cpp`, `coop_enemy.cpp`, `coop_combat.cpp`, `coop_context.cpp`,
  `coop_time.cpp`, `coop_entity_logic.h`, `coop_time_logic.h`, `clock.cpp/h`,
  `config.cpp/h`, `module.cpp`, `selftest_main.cpp`) and every touched vanilla
  file (`d_a_alink.cpp` + 3 `.inc`s, `d_a_alldie.cpp`, `d_a_e_ai.cpp`,
  `d_a_e_yc.cpp`, `d_a_kytag06.cpp`, `d_attention.cpp`, `d_cc_s.cpp`,
  `d_cc_uty.cpp`, `d_kankyo.cpp`, `f_ap_game.cpp`, `f_op_actor.cpp`,
  `f_op_actor_mng.h`, `d_a_player.h`, `sdk/include/mods/svc/game.h`). Read the
  full decision/review trail (`00-network`…`04-time-weather`, the Rev 3 plan,
  `m2-design-notes`, `TESTING.md`, and all `review-m*` files).
- **Live loopback NOT re-runnable here**: the TP RVZ + save are gitignored
  (`c4ca64a356`), so the final two-instance playtest is the user's. The
  net-off byte-for-byte-vanilla claim is verified by guard inspection (every
  net/coop path is `net.enabled`/`SessionLive()`-gated and every vanilla-file
  edit has an `#else` vanilla path), **not** by a live boot. This is the stated
  limitation of every review since M4.

---

## OVERALL VERDICT

**The implementation is coherent, buildable, and selftest-green.** The ten
milestones compose into a single end-to-end story (discovery → join →
spawn → replicate → fight → time/weather → room ownership → leave → host-leave
→ disable) with consistent seams: one `Session`, one `onGameFrame` pump,
`SessionLive()` as the universal gate, `SendGameMessage` as the one outbound
seam, `(stage, room)` as the universal authority key, and `#if TARGET_PC` +
`#else`-vanilla as the guard contract. The protocol is internally consistent
(16 types, v6, `WireSize` matches every serializer, `DeserializeMessage`
rejects unknown/size-mismatched/jointCount-overflow packets). The shutdown
order is correct (coop before net). The stay-put join, room-scoped EnemyEvent,
roster-refresh, weight-envelope recompute, per-entry cullMtx, and 1 Hz
TimeSync are all present and match the docs.

**One MAJOR cross-module bug** (a host-side use-after-free on enemy death,
invisible to every per-milestone review because it lives at the boundary of
the f_pc delete timing and the coop death poll) and one MAJOR-adjacent
behavioral seam (the non-owner freeze latency on room entry) are the only
things I'd want addressed before the live playtest. Neither blocks the build
or the selftest; both are findable in a live run. **Subject to those and the
live playtest, the branch is READY for M5 development** — the seams are clean
and documented.

---

## RANKED FINDINGS

### MAJOR 1 — host enemy-death poll dereferences a freed actor (use-after-free)

**Files**: `src/dusk/coop/coop_enemy.cpp` — `PollHostDeaths()` (the
`AdapterForActor(e.actor)` calls at the top of the scan loop and inside the
`dead`-resolution loop).

**The bug.** `EnemyEntry::actor` is a raw `fopAc_ac_c*` captured at
registration. The host's death poll runs in `enemy::onGameFrame`, which is
called from `duskExecute()` **before** `fpcM_Management()` in `fapGm_Execute`
(`src/f_ap/f_ap_game.cpp:844` then `:851`). `fpcM_Management` processes the
delete queue in `fpcDt_Handler()` — the **first** thing it does
(`src/f_pc/f_pc_manager.cpp:78`) — and only then runs `fpcEx_Handler` (actor
execute). So the per-frame order is:

1. `duskExecute` → `enemy::onGameFrame` → `PollHostDeaths` (derefs `e.actor`)
2. `fpcM_Management`: `fpcDt_Handler` (frees actors queued last frame) → creates → executes

Trace an enemy death:
- Frame N: enemy execute (step 2c) sets is_dead / `fopAcM_delete` queues it.
- Frame N+1 step 1: `PollHostDeaths` — the actor is **still alive** (not freed
  until step 2a this frame), `fopAcM_SearchByID(e.pid) == e.actor` → `gone=false`.
  `AdapterForActor(e.actor)` is safe this frame.
- Frame N+1 step 2a: `fpcDt_Handler` frees the actor. `e.actor` is now dangling.
- Frame N+2 step 1: `PollHostDeaths` — `fopAcM_SearchByID(e.pid)` returns
  nullptr → `gone=true`. **But `AdapterForActor(e.actor)` was already called
  at the top of the loop body**, on the freed pointer. `AdapterForActor` calls
  `fopAcM_GetName(const_cast<fopAc_ac_c*>(actor))`, which reads `actor->name`
  off freed memory. Then the `dead`-resolution loop calls
  `AdapterForActor(e.actor)` **again** to read `adapter->dropTableId`.

This is a use-after-free on every host enemy death, deferred ~2 frames. It is
masked in practice because the heap usually hasn't reused the slab yet (so the
name field still reads the old procName and the right adapter is found), which
is exactly why it survived ten per-milestone reviews — but it is UB, and a
heap reuser or a debug-allocation host (ASan/fill) would crash or report it.

**Fix (small, contained).** Store the per-type data that survives death on the
entry, not the actor. At `RegisterActor`, set
`e.adapter = AdapterForActor(actor)` (the adapter table is a `static
std::array`, lives forever — safe to hold). In `PollHostDeaths`:
- drop the top-of-loop `AdapterForActor(e.actor)`; use `e.adapter` for the
  `isDead`/`deathSwitchNo` calls (only reached when `!gone`, so `e.actor` is
  valid there);
- in the `dead`-resolution loop, use `e.adapter->dropTableId` instead of
  re-derefing `e.actor`.

`e.deathSwitch` is already captured alive in `hostOnExecuted` (post-execute,
actor alive), so the only death-time actor deref that remains would be the
`isDead` poll, which is guarded by `!gone`. After the fix, no `e.actor` deref
ever happens after the actor is freed.

**Severity**: MAJOR (latent host crash/corruption; manifests under heap
pressure or a sanitised build; fires on every enemy kill in the live game).

---

### MAJOR 2 — non-owner client's whitelisted enemies run native AI for up to ~15 frames on room entry

**Files**: `src/dusk/coop/coop_enemy.cpp` — `onGameFrame` (the
`g_frameCount % 15 == 0` scan cadence) + `puppetExecute` (the freeze gate is
`FindEntryForActor(actor) == nullptr → return false`).

**The bug.** On a room change, `onGameFrame` calls `ClearAll()` then re-registers
via `ScanAndRegister()` only when `g_frameCount % 15 == 0`. The receive-side
mitigation (`if (g_pidToEnemyId.empty()) ScanAndRegister();` in the
EnemySnapshot handler) only fires **after the first host snapshot arrives** —
and the host is itself on the same 15-frame scan cadence, so the host may take
up to ~15 frames to register the room's enemies and emit the first snapshot.
Until the client registers its local instances, `puppetExecute` returns `false`
for them, so the **non-owner client's local whitelisted enemies run their full
native AI** (aggro, move, attack, deal real damage to the client's Link) for
up to ~0.25 s after room entry. Then the first snapshot lands, the scan
triggers, the enemies freeze, and they snap to the host's pose/HP.

This is a real authority violation in the window: the client's Link can take
heart damage from an enemy the host's sim places elsewhere / at different HP,
and the client's local damage application is not forwarded as a CombatIntent
(`noteAtTgHit` checks `amIRoomOwner(tgRoom)` and returns early on the owner —
but on the **non-owner**, the un-frozen local enemy's own collision pass deals
real damage directly, bypassing the intent path entirely because the enemy
isn't registered yet so the `isRegistered(tgActor)` gate in `noteAtTgHit`
also fails). The damage sticks.

It is a window, not a permanent desync (the freeze snaps HP from the host
next snapshot), and the heart loss is per-player (the client's own save) so it
doesn't corrupt the host — but it is the one place the "non-owner never sims
the room's enemies" invariant is violated, and a player who walks into a room
and gets hit by an enemy that should have been frozen is a visible playtest
bug.

**Fix options.**
- (a) Cheapest: on the room-change reset in `onGameFrame`, run `ScanAndRegister()`
  immediately (not gated on `% 15`) so local instances are registered — and
  thus frozen by `puppetExecute` — on the first frame in the new room. The
  host snapshot then drives them. (The 15-frame cadence stays as a backstop
  for mid-room spawns.)
- (b) Tighter: in `puppetExecute`, when `SessionLive() && !OwnsRoom(roomNo) &&
  isWhitelistedType(name) && !isRegistered`, still freeze (return true, hold
  spawn pose) instead of falling through to native AI — i.e. freeze
  *optimistically* for whitelisted types in a non-owned room, and let
  registration backfill the snapshot.

Either keeps v1 semantics. This is the highest-value live-playtest probe (see
below).

**Severity**: MAJOR-adjacent (correctness window on every room entry on a
non-owner; masked on the host's own room because the owner *should* sim
natively).

---

### MINOR 1 — `PlayerState` wire size is 2017 B, not the documented 2001 B

**Files**: `include/dusk/net/protocol.h` (`PlayerStateWireSize()` comment +
the `WireSize(MsgType::PlayerState)` comment "= 2001"), `docs/design/mod-coop/00-network.md`
§6 ("8 × 2001 B × 60 ≈ 0.96 MB/s"), `docs/design/mod-coop/02-player-state.md`
§1 ("~2.0 KB").

`PlayerStateWireSize()` = `5 + 16 + 5 + 10 + 1 + 12 + sizeof(Mtx) + 40*sizeof(Mtx)`
= `5+16+5+10+1+12+48+1920` = **2017** with TP's 3×4 `Mtx` (48 B, confirmed
`libs/dolphin/include/dolphin/mtx.h:22: typedef f32 Mtx[3][4]`). The selftest
asserts `WireSize(PlayerState) == PlayerStateWireSize()` (both 2017), so it is
internally consistent — but the human-readable "2001" figure in two code
comments and the design doc is 16 B off. The `kMaxMessageSize = 4096` comment
"fits the Rev 3 D4 raw matrix pose (~2.7 KB)" is stale (it's ~2.0 KB now).
Bandwidth: 8 × 2017 × 60 ≈ 945 KB/s — the "~1.0 MB/s" headline still holds.

**Fix**: replace `2001` → `2017` in the two comments and `00-network.md` §6,
and `~2.7 KB` → `~2.0 KB` in the `kMaxMessageSize` comment. (No wire change;
no version bump.)

---

### MINOR 2 — `CombatResult` is dead wire traffic in v1; `EnemySnapshot.aggro`/`.type` are sent but never applied

**Files**: `src/dusk/coop/coop.cpp` `OnGameMessage` (`CombatResult` branch is a
comment-only no-op), `src/dusk/coop/coop_enemy.cpp` `puppetExecute` (reads
`snap.pos/angle/speed/health/flags/anim`; never reads `snap.aggro` or
`snap.type`).

`hostOnExecuted` builds and star-relays a `CombatResult` for every injected
hit, but the client receive side deliberately does nothing ("the authoritative
HP always rides the next EnemySnapshot; nothing to apply client-side in v1").
`EnemySnapshot.aggro` (the nearest-player hint, computed via
`resolveNearestPlayer` on the host) and `EnemySnapshot.type` (procName) are
populated on the host and serialized but never read by the client apply path
(the client already has the local actor with its own type, and a frozen puppet
doesn't aggro). These are 1 + 2 + 2 = ~5 bytes/enemy/frame of dead bandwidth
plus a `resolveNearestPlayer` scan per enemy per frame on the host.

Not a bug — the design doc says the result ack is "optional" and aggro is a
"hint" — but a fresh engineer will grep for the consumer and find none. Either
wire a consumer (M5 interpolation / PvP) or drop the fields and save the
`resolveNearestPlayer` cost. Documented here so M5 doesn't re-derive it.

---

### MINOR 3 — `03-enemies.md` §3.4 still lists `E_FB` ("Fire Baba") as a candidate; the shipped whitelist drops it

**Files**: `docs/design/mod-coop/03-enemies.md` §3.4 (line ~240: "Fire Baba
(`E_FB`): forced `health = 0` by interaction types"), vs
`docs/design/mod-coop/m2-design-notes.md` §2/§5 and the shipped
`kAdapters` table in `coop_enemy.cpp`.

`m2-design-notes.md §2` corrected `E_FB` to "Freezard" (not Fire Baba) and
**dropped** it (iron-ball-only damage, `GetTgHitAc()` casts to `daObjCarry_c*`,
reads slot 0 — unsafe for synthetic injection). The shipped whitelist is
`{E_AI, E_HM, E_DF, E_YC, E_MD, B_TN}`. `03-enemies.md` is the *investigation*
doc (pre-M2) and was never updated, so its §3.4 candidate list
(`E_AI/E_HM/E_FB/E_DF/E_YC/E_MD + B_TN`) and its "Fire Baba" naming are stale.
`m2-design-notes.md §5` is the authoritative final record. A fresh engineer
reading `03-enemies.md` first will be confused.

**Fix**: add a one-line "SUPERSEDED — see m2-design-notes.md §2/§5" banner at
the top of `03-enemies.md §3.4` (and §2's candidate list), or strike `E_FB`
there. Same for the `E_GS` entry (dropped per §2).

---

### MINOR 4 — `PlayerEventMsg.scene` semantic changed in M4.5 without a wire-layout note

**Files**: `include/dusk/net/protocol.h` `PlayerEventMsg.scene` doc-comment +
the v6 bump comment (which attributes v6 solely to `RoomOwnership`).

The v6 bump was for `RoomOwnershipMsg` (a new type — a real layout change).
But M4.5 also redefined the meaning of the existing `PlayerEventMsg.scene`
byte (now "1 = same-stage move, 0 = cross-stage / not a SceneChange" — the
host's room-table sniff keys on it). That is a *semantic* change to an
existing field, not a layout change, so no bump was required — and the
`scene` doc-comment documents it. But the `kProtocolVersion` history comment
only mentions `RoomOwnership` for v6; a fresh engineer diffing an old v6
capture (M4, pre-M4.5) against HEAD would see the same bytes and misread
`scene`. On this branch there is no shipped v6-without-M4.5, so it's fine —
but the version history should note the M4.5 `scene` semantic so the next
semantic change is a conscious decision.

**Fix**: add a line to the `kProtocolVersion` v6 comment: "M4.5 also
redefined `PlayerEventMsg.scene` as the same-stage flag (semantic only; no
layout change)."

---

### MINOR 5 — stale room-ownership entries can leak on a stage-transition race

**Files**: `src/dusk/net/session.cpp` `UpdatePlayerRoom` + `setLocalRoom`.

`LocalStageName()` (`dComIfGp_getStartStageName()`) and the real Link's
`fopAcM_GetRoomNo` can disagree for a frame during a stage transition (the
start-stage object is re-pointed on stage load, the Link is recreated). If
`setLocalRoom` runs with the new room number but the old stage name (or vice
versa), `UpdatePlayerRoom` calls `OnPlayerLeave(oldStage, oldRoom)` then
`OnPlayerEnter(staleStage, newRoom)`, creating a bogus `(staleStage, newRoom)`
entry owned by the host. The next frame corrects the host's real entry but
the bogus entry is never `OnPlayerLeave`'d (it only fires for the player's
*current* room), so it lingers in `ownership_.rooms_` until the session ends.

It is harmless in practice (no one routes intents to `(staleStage, newRoom)`
because no player is ever there; `OwnerOf` for the real `(newStage, newRoom)`
is correct), but it is a slow leak on repeated stage transitions and a
fresh-engineer puzzle. The `setLocalRoom` call site in `onGameFrame` is
already guarded by `g_realLinkReady && g_realLink != nullptr`, which narrows
but does not eliminate the race window.

**Fix**: in `UpdatePlayerRoom`, if `cur.room >= 0` and the stage changed,
always issue `OnPlayerLeave(cur.stage, cur.room)` for the *previous* key
before adopting the new one (already done) AND, if the new stage differs from
`cur.stage`, clear any host-owned entry for `(cur.stage, cur.room)` explicitly
(sticky entries for a stage the player is definitely leaving should not
survive a stage change). Or, cheaper: sweep `ownership_` for rooms whose
`order` is empty on each `setLocalRoom` and drop them.

---

### MINOR 6 — `sendPlayerState` relies on the puppet early-return for correctness, not an `isPuppet` guard

**Files**: `src/d/actor/d_a_alink.cpp:19094` (the `sendPlayerState(this)` call
near the end of `execute`), `src/dusk/coop/coop.cpp` `sendPlayerState`.

A puppet's `execute` returns at the top (`if (isPuppet) return puppetExecute``,
line 17963), so it never reaches the `sendPlayerState` call at 19094. The
sender itself has no `isPuppet` check — it relies on that ordering. This is
correct today, but it is the kind of implicit coupling that breaks if someone
later moves the sender call or makes `puppetExecute` not return early.
Defense-in-depth: add `if (dusk::coop::isPuppet(link)) return;` at the top of
`sendPlayerState` (one line, matches the `puppetDrawHidden`/`modelCallBack`
pattern).

---

### MINOR 7 — `g_roomClear[256]` / `g_roomHadEnemies[256]` indexed beyond the real room range

**Files**: `src/dusk/coop/coop_enemy.cpp` (`bool g_roomClear[256]`,
`g_roomHadEnemies[256]`, the RoomClear receive gate `ev.enemyId < 256`).

Room numbers are `s8` (−128..127); the arrays are sized 256 and indexed by
room number (0..127 in practice — registration skips `roomNo < 0`). The
RoomClear receive-side gate checks `ev.enemyId < 256`, so a forged/malformed
`EnemyEvent(RoomClear)` with `enemyId` in 128..255 writes `g_roomClear[128..255]`,
a slot no `clientRoomClearGated(s8)` ever reads. No crash, no behavior change
(`clientRoomClearGated` indexes by the real `s8 roomNo` 0..127). It's a
robustness nit: tighten `< 256` to `< 128` (or to the stage's real room count)
so a malformed packet can't write past the meaningful range, and document that
the array is over-sized by 2× for the s8 sign.

---

### MINOR 8 — `OnWorldInit` roster-refresh overwrites the client's `worldStage_` (harmless today, but surprising)

**Files**: `src/dusk/net/session.cpp` `OnWorldInit` (overwrites `worldStage_`
on every roster-refresh re-broadcast), `src/dusk/coop/coop_time.cpp`
`SeedTargets` (reads `worldTime()`/`worldWeather()` but not `worldStage()` on
a client).

On every later join the host re-broadcasts `WorldInit` to existing clients
(the roster-refresh). `OnWorldInit` unconditionally overwrites the client's
`worldStage_` with the host's current stage. No client consumer reads
`worldStage()` (the puppet gate keys on the remote's *real* stage from
`PlayerState.stage`, not `worldStage`; the time module reads `worldTime`/
`worldWeather`), so it's harmless today. But it's a latent foot-gun: if M5
ever reads `worldStage()` on a client (e.g. a "where is the host" UI marker),
a 3rd-player join would snap it. Worth a comment in `OnWorldInit` noting that
`worldStage_` on a client is only the join-time reference, or scope the
`worldStage_` write to the joining peer only.

---

## VERIFIED-OK (the big-ticket claims I confirmed end-to-end)

- **Forced clean rebuild is green.** `ninja -t clean` (2267 files) → full game
  + selftest link with **zero net/coop warnings**; the only warning is the
  pre-existing vanilla `JKRExpHeap.h:30` multichar. Selftest **318/318 PASS**,
  exit 0.
- **Protocol consistency.** All 16 `MsgType`s have `WireSize` matching their
  serializer field-by-field; `DeserializeMessage` rejects unknown types
  (`typeRaw < JoinRequest || > RoomOwnership`), size mismatches
  (`payloadSize != WireSize(type)`), and `jointCount > kMaxJoints`
  (semantic validation). `kProtocolVersion = 6`. `ChannelFor` routes
  PlayerState/EnemySnapshot/TimeSync to unreliable, everything else to
  reliable. The determinism sweep covers all 16 types.
- **Transport thread safety + ring policies.** Channel-split SPSC rings
  (reliable 64, snapshot 128); reliable `Push` fails explicitly (counter +
  log), snapshot `PushOrReplace` keeps freshest. Peer-slot generation bumped
  on assign AND release; `Send` stamps generation, `Poll`/`DrainOutbox` drop
  on mismatch — a stale packet can never misdeliver to a reused slot. Socket
  thread is the only writer of `peerSlots_`; the game thread reads
  `peerGenerations_` (atomic). `Stop` is idempotent, joins the thread, and
  destroys the host on the socket thread (`enet_host_destroy` after the
  service loop).
- **Shutdown order.** `dusk::config::shutdown()` calls `dusk::coop::shutdown()`
  (which `Session::Stop()`s → sends PlayerLeave/SessionEnd → joins transport)
  **before** `dusk::net::shutdown()` (`config.cpp:650/655`). The M1 MAJOR-4
  ordering bug stays fixed. `coop::shutdown` also stops both discovery threads
  and clears enemy/time per-stage state.
- **Guard hygiene (the byte-for-byte-vanilla contract).** Every touched
  vanilla file uses `#if TARGET_PC` with an `#else` that keeps the original
  call: `f_op_actor.cpp` `fopAc_Execute` (the `#else` is the direct
  `fpcMtd_Execute`), `d_kankyo.cpp` `setDaytime`/`dKy_Execute`/`dKy_Create`,
  `d_a_alink.cpp` execute/draw/`modelCallBack`/create/destructor, `d_a_alldie.cpp`,
  `d_a_e_ai.cpp`/`d_a_e_yc.cpp` (6+3 targeting sites), `d_attention.cpp`,
  `d_cc_s.cpp`, `f_ap_game.cpp`. The D3 header-inline re-routing
  (`f_op_actor_mng.h` `fopAcM_searchPlayer*`/`fopAcM_getContextPlayer`,
  `d_a_player.h` `daPy_getPlayer*ActorClass`) keeps the `#else` vanilla
  (`dComIfGp_getPlayer(0)`) and falls back to slot-0 outside a
  `ScopedEnemyTarget` scope (`currentTargetPlayer()` returns slot-0 when the
  thread_local stack is empty), so non-enemy code paths are vanilla. With
  `net.enabled=false`, `SessionLive()` is false everywhere → every coop hook
  (`puppetExecute`, `hostNeedsContext`, `hostOnExecuted`, `sendPlayerState`,
  `clientClockReplica`, `clientWeatherForce`, `suppressDiceWeather`,
  `noteAtTgHit`, `clientRoomClearGated`, `hostRoomCleared`) early-returns;
  `isPuppet` is false (no puppets registered) so the execute/draw/modelCallBack
  guards are no-ops. (Caveat: verified by inspection, not a live boot — no
  RVZ.)
- **Stay-put join (M4.5 MAJOR 1).** No `dStage_changeScene` /
  `dComIfGp_setNextStage` / `setRestartRoom` in `src/dusk/coop/`; the
  `worldStage_` carry is retained and filled every frame from the real Link
  (`coop.cpp` `onGameFrame`); the puppet visibility gate keys on
  `PlayerState.stage` (the remote's real stage), not on a warp decision. The
  `SceneChange` event's `scene` flag gates the room-table sniff to same-stage
  moves only.
- **Room ownership (M4).** `RoomOwnershipTable` implements sticky first-in,
  host-defaults-own-its-room (`RecomputeOwner` with `isHost`), transfer on
  leave/disconnect (`OnPlayerDisconnect`), no ping-pong on arrival.
  `RouteCombatIntent` routes to `ownership_.OwnerOf(attackerRoom)` (or host
  consumes if ownerless/host-owned). `EnemySnapshot` + `EnemyEvent` are
  `RoomScoped` on relay (`SendToAllInRoom`) AND receive-side gated
  (`snap.enemyId < kDynamicIdBase && (enemyId>>8) != localRoomNo()` for
  snapshots; the same for `Died`; bare room for `RoomClear`). Entity-id
  stability: stage keys `(roomNo<<8)|setID` + owner-major dynamic ids
  (`kDynamicIdBase | (owner<<12) | counter`), tested in the selftest.
- **Roster refresh (M1 MAJOR).** On a successful join the host sends
  `WorldInit` (full roster) to every already-joined peer except the new
  player; `ApplyRoster` rebuilds the roster cleanly. Selftest covers 3-player
  visibility.
- **Puppet create neutralization (M1 BLOCKER B1).** Puppets call
  `procWaitInit()` instead of `setStartProcInit()`; the entry-dependent wait
  terms (`isHorseStart`/`checkCanoeStart`/`checkBoarStart`/startPoint==-4)
  are suppressed; `setRestartRoom`/`setSelectEquipClothes`/`setTransformStatus`/
  `setDamagePoint`/`clearPlayerStatus`/slot-0 pointer restore are all guarded
  by `isPuppet`; the destructor's slot-0 clears are guarded. Create-phase
  deadline (`kCreateDeadlineFrames = 600`, 3 strikes) prevents a stuck create
  from locking `g_createInFlight` forever.
- **Weight-envelope recompute (M1 MAJOR render fix).** `ApplyPuppetState`
  calls `mpLinkModel->calcWeightEnvelopeMtx()` after the per-joint `setAnmMtx`
  paste and records the WEvlp matrices for frame interp — the fix for
  "some vertexes not following the rest of the animation". `modelCalc` is
  called per-Link to re-assert the shared-`J3DModelData` mtxCalc pointers
  (risk 2). `modelCallBack` is short-circuited for puppets (M3.5
  defense-in-depth).
- **Per-entry cullMtx (M2.5 MAJOR-2).** Each `EnemyEntry` has its own `Mtx
  cullMtx`; `puppetExecute` points `fopAcM_SetMtx(actor, &e->cullMtx)` every
  apply, so multi-puppet rooms cull on each puppet's own position (not the
  last writer's).
- **Time/weather (M3/M3.5).** `g_syncClock = NetClock::AtRate(1)` (the real
  1 Hz gate); absolute-phase adopt on `TimeSync`; `SeedTargetsDecision` gates
  time adoption on `!g_time.valid` so a roster-refresh `WorldInit` cannot
  regress a fresher `TimeSync` (glm MINOR 1); weather re-seed is safe
  (`WorldInit`/`WeatherChange` are reliable-ordered on the same channel, so
  `WorldInit`'s weather is never staler than the last `WeatherChange` the
  client processed); darkworld branch runs the local twilight clock; thunder
  edge publish (glm M3.5 MINOR 1); pond 2x and wolf-howl fast-forward rate
  buckets.
- **LAN discovery (M4).** Announcer (2 s broadcast to LAN + loopback) +
  listener (44771, `SO_REUSEADDR`); player count seeded before the thread
  starts (M4.5 MINOR 6); listener logs "discovery: found session ...";
  `sessions_` mutex-protected; `g_activeListener` atomic; both threads join on
  `Stop`.
- **Duplicate-join / JoinAccept validation (M0.5).** Duplicate `JoinRequest`
  ignored; `JoinAccept` rejects out-of-range ids and rosters that don't mark
  the id present; oversized inbound packets counted and dropped at the socket.
- **D3 targeting context (M2.5).** `ScopedEnemyTarget` pushed around the host's
  whitelisted enemy execute (the lambda in `fopAc_Execute`); `resolveNearestPlayer`
  considers the host Link + non-hidden puppets; E_YC 6/6 and E_AI 3/3 direct
  slot-0 reads routed; `hostNeedsContext` is owner-gated so clients never push.

---

## TOP 10 THINGS FOR THE LIVE PLAYTEST

(What to specifically probe when you run two instances — the selftest can't
cover these because the RVZ is gitignored.)

1. **Host kills many enemies of EVERY whitelist type, including B_TN.** This
   is the MAJOR 1 probe — watch for a host hang/crash ~2 frames after each
   enemy death (the `PollHostDeaths` `e.actor` deref on freed memory). Under a
   normal heap it won't crash; under ASan or a fill-pattern debug heap it
   will. If it doesn't crash, the drops + switches still land correctly (the
   UAF reads the old procName, which is usually intact) — so green here does
   NOT prove the bug absent; run a sanitised build if possible.
2. **Client walks into a room with whitelisted enemies (non-owner).** Watch
   the first ~0.25 s: do the enemies move/attack before snapping to the
   frozen pose? Does the client's Link take damage in that window? (MAJOR 2.)
   Repeat for each whitelist type and for B_TN.
3. **Combat end-to-end.** Host fights an enemy → client sees the same HP; a
   client's hits land and kill (host applies); drops spawn on BOTH; doors
   open together (ALLDIE gate — verify a room with synced enemies does NOT
   open on the client until the host's `RoomClear` bit arrives, and DOES open
   once it does).
4. **Two players in different rooms of the same stage.** Each room's enemies
   sim on that room's owner (first in; host wins its own room); the other
   player sees them frozen; combat from either side lands via the room-owner
   route; the owner leaving transfers the room (enemies re-sim on the new
   owner — expect dead enemies may resurrect, per the accepted design).
5. **Stay-put join (M4.5).** Client joins a host in ANOTHER stage → both
   players stay in their own stages (no warp, no toast, no yank-back leash);
   puppets stay hidden until both travel to a shared stage; once they meet,
   puppets appear and combat/ownership work. `USA/Card A/*.gci` mtime
   untouched on both sides.
6. **Host leave / `kill -9` the host process.** Client shows "Host
   left"/"Host disconnected" toast (distinguish the two reasons), puppets
   despawn, single-player continues normally. Then disable `net.enabled` on
   the client and confirm vanilla single-player.
7. **Time & weather.** Same sky on both; rain arrives on both (watch the
   ramp, not just the pin); a cutscene freezes the clock on both; a stage
   transition re-asserts the same time + weather; Fishing Pond (`F_SP127`/
   `R_SP127`) runs 2x; a wolf-howl fast-forwards and the client follows.
8. **Form swap.** Host in wolf, client in human (or vice versa) → puppet
   form-swap: the other form's arc loads on demand, `changeWolf`/`changeLink`
   runs, no host-save `setTransformStatus` write (save mtime). Verify the
   weight-envelope recompute looks right (no "floating feet" / mismatched
   shoulders on the puppet).
9. **3+ players.** Earlier joiners see later joiners (roster refresh —
   puppets spawn for both directions); a `CombatIntent` from player A against
   an enemy in player B's owned room routes to B (not the host); a 3rd-player
   join does not regress the existing clients' clock (the `SeedTargetsDecision`
   gate) or weather.
10. **Rapid room bouncing + discovery.** Bounce between two rooms of the same
    stage: ownership is stable (no ping-pong), no puppets get stuck hidden,
    no bogus ownership entries accumulate (MINOR 5 — check the host log for
    stray `room ... owner -> ...` lines for rooms no one is in). And: with
    the host running, the client log shows `discovery: found session ... at
    <ip>:<port> (players n/m)` and the Settings → Network tab lists it; manual
    `net.joinHost` IP join works as the fallback.

---

## TOP 10 RECOMMENDATIONS FOR M5

(Seams the next developer needs — waves, horses, PvP, interpolation.)

1. **Fix MAJOR 1 first.** Store `const NetEnemyAdapter* adapter` (or `s16
   procName`) in `EnemyEntry` at registration; never deref `e.actor` after
   `gone`. M5 adds more enemy types / death paths (waves), so the UAF surface
   grows — fix it before extending the whitelist.
2. **Fix MAJOR 2 (freeze latency) before waves.** Waves arriving mid-room
   need the freeze to be immediate. Either scan on room change (option a) or
   freeze optimistically for whitelisted types in a non-owned room (option b).
   The receive-side `if (g_pidToEnemyId.empty()) ScanAndRegister()` trigger
   should also fire on `EnemyEvent(Spawned)` (currently it only fires when
   the map is empty — a wave spawning into an already-populated room won't
   trigger a scan).
3. **Dynamic-spawn path is dormant.** `registerDynamicEnemy` +
   `EnemyEvent(Spawned)` exist but v1 is stage-placed only; the client
   `Spawned` branch logs and does NOT create (the client can't reproduce
   owner params today). M5 waves need a wire params field + a client spawn
   path. The owner-major dynamic-id scheme (`coop_entity_logic.h`) is ready.
4. **`CombatResult` receive-side is a no-op.** If M5 needs authoritative HP
   acks (interpolation, PvP kill credit), wire a consumer in
   `coop.cpp OnGameMessage`. Currently it's star-relayed dead traffic. Either
   use it or drop it + the `hostOnExecuted` send.
5. **`EnemySnapshot.aggro`/`.type` unused on apply.** M5 enemy-pose /
   interpolation may want `aggro`; or drop both to save ~5 B/enemy/frame +
   the per-enemy `resolveNearestPlayer` scan on the host. Decide before
   adding the matrix-copy enemy pose (the bandwidth grows fast).
6. **The D3 header-inline re-routing is fork-wide.** Any new enemy type in
   M5 that reads the player via `fopAcM_searchPlayer*` /
   `daPy_getPlayer*ActorClass` gets targeting-context routing for free — but
   per-type *read-coverage* verification (the `m2-design-notes.md §2.1` audit:
   grep the TU for `dComIfGp_getPlayer(0)` direct reads and route them) is
   still required per new type. Don't assume the inlines cover everything.
7. **Room-clear state is per-room and reset on every room change (`ClearAll`).**
   M5 wave/wipe mechanics may need a persistent room-clear lifecycle separate
   from the player's room visits (a wave room that re-populates after clear
   needs the bit to reset, which `ClearAll` does on re-entry — but a
   mid-room wave re-pop won't reset `g_hostRoomClearSent`). Consider a
   per-room `RoomClearState` object keyed by `(stage, room)` rather than the
   flat `[256]` arrays.
8. **Horse entity channel.** `PlayerEventId::Mount`/`Dismount` are reserved
   stubs; `mRideStatus` is carried in `PlayerState.stateFlags` but no horse
   puppet exists. The spawn pattern (`fopAcM_create(fpcNm_HORSE_e, …)` per
   remote, puppet-drive via the same `setAnmMtx` trick) is the M1 pattern
   applied to a new procName — clean seam in `PlayerEventId` + the puppet
   registry (which is currently `daAlink_c`-only; generalising it to hold a
   horse pid per remote is the work).
9. **PvP.** `setDamagePoint`/`setDamagePointNormal`/`setLandDamagePoint` and
   `daAlink_tgHitCallback`/`daAlink_coHitCallback` are the puppet-damage
   suppression seam (currently puppets skip all damage). PvP flips those into
   intent senders; `coop_combat::noteAtTgHit` is the routing seam (it already
   rejects `targetPlayerId != kInvalidPlayerId` as "friendly fire off" — M5
   removes that reject and routes a player-target intent to the target's
   machine). The `CombatIntent.targetPlayerId` field is already on the wire.
10. **Protocol versioning discipline.** The M4.5 `scene` semantic change
    (MINOR 4) shows that non-layout changes don't bump `kProtocolVersion` —
    establish for M5 whether a new `EnemyEvent` data packing (e.g. wave id
    in `data`) bumps or not. The `DeserializeMessage` exact-size check means
    any field addition/removal MUST bump; a semantic reinterpretation of an
    existing field is a judgment call. Document the rule in `protocol.h` so
    M5 doesn't ship a silent incompatibility.

---

## Coherence (could a fresh engineer build and run this from the docs alone?)

**Yes, with one pointer to the authoritative record.** `TESTING.md` gives
the exact build + run commands (and the `tools/run-coop.sh` helper wraps the
two-instance loopback); `00-network.md` is the transport/session/protocol
spec and matches `protocol.h` normatively; `implementation-plan.md` Rev 3 +
its §9 M1-resolution table and the M4/M4.5 status blocks record every
decision and every per-milestone fix. The one trap is `03-enemies.md` (the
pre-M2 investigation), whose §3.4 whitelist/candidate list is stale
(MINOR 3) — `m2-design-notes.md §2/§5` is the authoritative whitelist, and a
fresh engineer should read that first. The `review-m*` trail is the full
decision archaeology; a fresh engineer can trace any "why" back to a review
finding + its fix commit. The M5 seams (above) are where the next developer
plugs in; they're documented in `coop.h`/`coop_enemy.h` public APIs and the
`PlayerEventId`/`EnemyEventId` enums.

**Doc-vs-code accuracy at HEAD**: `TESTING.md` status block, the plan M4/M4.5
status blocks, `00-network.md` §4/§5/§6 (modulo the 2001→2017 figure, MINOR 1),
`04-time-weather.md` (the M3.5 fixes are recorded), and `m2-design-notes.md`
are all accurate to HEAD. The stale items are `03-enemies.md` §3.4 (MINOR 3)
and the two `2001`/`~2.7 KB` comments (MINOR 1) — none affect build/run.

**Bottom line**: a coherent, well-documented, buildable, selftest-green v1
with two MAJOR cross-module findings (one latent UAF, one freeze-latency
window) to address before/within the live playtest, and clean seams for M5.
