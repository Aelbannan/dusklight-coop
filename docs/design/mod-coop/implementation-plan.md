# Implementation plan — networked co-op (Rev 3, fork-based)

Status: **Revision 3 — decision: full fork, not a mod.** Sources: `00-network.md` (transport/session),
`01-puppet-link.md`…`04-time-weather.md` (mechanical investigations — their *findings* carry over;
their "hook from a mod" framing becomes "`#if TARGET_PC` guard in source"), `review-kimi-k3.md` +
`review-glm-5.2.md` (adversarial pass — validated the mechanics; their mod-specific blockers are
moot under the fork), `docs/design/network.md` (design decisions).

## Revision 3 — the pivot (and why)

The user requirement "enemies must be able to target remote players" is **infeasible from a pure
`.dusk` mod** (enemy AI reads player-0 through inline accessors compiled into 100+ files) and
feasible in a fork via the existing per-actor context swap. Decision: **the whole co-op lives in
the fork** (`#if TARGET_PC` guards, per AGENTS.md conventions), targeting the fork binary.
Compatibility with upstream is maintained the way this repo already does it: vanilla sources
untouched, coop code additive and guarded, coop branch rebased on upstream main.

Every mod-specific blocker from the adversarial review dissolves in-source:

| Review item | As a mod | As a fork (Rev 3) |
|---|---|---|
| Enemy targeting of remotes | Impossible (inlines) | **Context swap** — `fopAcM_getContextPlayer()` extended from NPCs to enemy AI: enemies aggro the nearest real player |
| R2 `dComIfGs_setRestartRoom` inline | Unhookable | Guard the call site in `daAlink_c::create` |
| R4 shared `bgWaitFlg` create race | Serialize spawns | Per-slot state (fork already has this) |
| R1 reload crash on `sub_method` | Blocker | Moot — no runtime unload |
| R3 fork/stock ABI hazard | Real | Moot — same binary (fork still bumps `GAME_SERVICE_MAJOR` as hygiene) |
| R13 hook-name/symbol fragility | Load-time failures | Moot — compile-time |

The network architecture (pose sync, enemy freeze/apply, combat validation, time/weather,
protocol) is **unchanged** — it's orthogonal to fork-vs-mod. Only the attachment changes.

## Revision 3 — decisions locked (from review)

| # | Decision | Choice |
|---|----------|--------|
| D1 | Puppet update | **Frozen + live colliders** — guarded early-return in `execute` for puppets (simpler than `sub_method` in-source); per-frame collider re-registration |
| D2 | Cadence | **Everything 60 Hz** (players + enemies, every frame) |
| D3 | Enemy targeting | **Full retargeting via context swap** — the fork's per-actor player resolution, so host enemies aggro the nearest real player (host Link or remote puppet) |
| D4 | Pose format | **Raw matrices** (2.67 KB/player/frame) — quantization struck from the future list |
| D5 | Drops | **Client-side spawn** on `EnemyEvent(died)` + death-presentation beat |
| D6 | Join policy | **Stay-put (warp removed by user decision)** — a joining client stays in its own stage; players meet by traveling to a shared stage |
| D8 | Host migration | **None v1** — session ends on host leave; UX decided in M4 |
| D9 | Boss union-bit | **Deferred** — v1: boss exists iff the room owner's story needs it |
| D10 | Scale flags | **Ship now** (5 B/frame + per-joint `setScaleFlag`) |
| D11/D12 | Repo | **In-repo** (`mods/`-style dir inside the fork, built by the fork's CMake) — not a standalone pinned repo; SDK remains available but unused for this project |
| D7 | Horse | Inline horse state in the player packet (separate channel = M5) |

## 1. Verdicts at a glance

| Area | Verdict |
|------|---------|
| Puppet Link | **Yes** — spawn machinery already exists in the fork (`coop_player.cpp:338+`, indexed-actor `TARGET_PC` blocks); freeze/apply per 01/02 mechanics |
| Player state | **Yes** — matrix pose via `J3DModel::setAnmMtx` (interp-aware, R15) |
| Enemy authority | **Yes** — `fopAc_Execute` freeze via in-source guard; combat validation per 03; **plus full target retargeting via context swap** |
| Time & weather | **Yes** — 04 mechanics; rate model now trivial (fork owns the wolf-howl code, R8 moot) |
| Networking | **Yes** — ENet vendored into the game build; everything 60 Hz |

## 2. Architecture

- **Target**: the fork binary. Coop code lives in `src/dusk/net/` + `include/dusk/net/` (network
  layer) and guarded `#if TARGET_PC` blocks in vanilla files (attachment points). Vanilla code is
  untouched; guards follow AGENTS.md / `docs/code-conventions.md`.
- **Upstream compatibility**: coop branch rebased on upstream main; `#if TARGET_PC` keeps the
  non-PC path vanilla; guard hygiene is the contract. Fork hygiene: bump `GAME_SERVICE_MAJOR`
  (the fork grew game structs) — protects the SDK surface even though the coop no longer uses it.
- **Reuse from the existing fork** (don't rebuild): `Runtime`/per-player state, `ScopedContext` +
  `fopAcM_getContextPlayer()` (extends to enemy targeting), the `enemy` subsystem's
  `EnemyAdapter` whitelist + clone spawn, `combat` hit/cut-type logic, `save` per-player records,
  the indexed-actor spawn path.
- **Retired by the network model** (the current fork's split-screen machinery): `render`
  multi-viewport, `camera` per-view, `input` multiplexing, `event` party-gather machinery —
  each machine runs one viewport, one input, per-triggerer story.
- **Topology**: star — world host relays all player state; sim owner (v1: host in its own room)
  owns enemies and validates combat. Threads: socket thread ↔ SPSC rings ↔ game thread.

## 3. Cross-cutting rules

1. **Session entity ids, never fpc ids.** Player = session `PlayerId` (0–7). Enemy = per-stage
   `(roomNo, setID, procName)` or owner-assigned counter. Registries are per-stage. `0xFFFF` =
   none/local-only for attention targets.
2. **Puppets are passive.** No story logic, no host-save writes (guard the create's
   `setRestartRoom`/`setSelectEquipClothes` for non-authority Links), no host damage (guard
   `setDamagePoint`), never "the Link" (slot-0 pointers restored after create).
3. **Sender reads the local real Link; puppet path only applies.**
4. **Everything 60 Hz, no interpolation v1** (Anchor model). Stale/frozen remote = seq-gap
   timeout → freeze last pose.
5. **World state stays per-player.** No flag/item/check sync, ever.
6. **Lifecycle**: puppets despawn on stage change (existing `dScnPly_Delete`/`dScnRoom_Delete`
   handling + re-spawn flow) and on session end; no runtime unload to worry about.

## 4. Project structure

```
src/dusk/net/                 # network layer (new)
├── transport.cpp/h           # ENet thread, SPSC rings (enet.c vendored in third_party/)
├── session.cpp/h             # host/client state machine, PlayerId assignment
├── protocol.h                # message structs + hand serializers (00-network.md §5)
└── clock.cpp/h               # 60 Hz send cadence, snapshot builders
src/dusk/coop/…               # existing subsystems: reuse (Runtime, context, enemy, combat, save)
include/dusk/net/             # headers
third_party/enet/             # vendored ENet (zlib)
# guarded attachment points in vanilla files (see M1/M2 hook → guard lists)
```

## 5. Milestones

### M0 — Baseline + netcode skeleton

- Rebase the coop branch on latest upstream main; verify the existing in-tree coop still builds
  and plays (the network model will retire parts of it, but it's the regression baseline).
- Vendored ENet wired into the build; `src/dusk/net/` skeleton: transport thread, SPSC rings,
  protocol structs + serializers, session/PlayerId scaffolding, config vars (host/join IP,
  session name).
- Fork hygiene: bump `GAME_SERVICE_MAJOR` (R3 residue).
- **Accept**: mod-free build runs; netcode skeleton compiles; LAN host/client handshake works
  with no game code attached.

### M1 — Player replication

Adapts the fork's existing multi-Link machinery to network puppets (01/02 mechanics).

- **Spawn**: reuse the indexed-actor path (`coop_player.cpp:338+`); per-slot create state (R4
  already solved in-fork); create neutralization as guards in `daAlink_c::create` (restart-room,
  select-equip, Midna/NPC/ride-actor fan-out suppressed for non-authority Links).
- **Freeze**: guarded early-return in `daAlink_c::execute` for puppet instances (keep collider
  registration live — D1); `modelCalc` mtxCalc re-assert per Link; `setDamagePoint` guard.
- **Apply** (`puppetUpdate`, from 01 §6.4 + 02 §4 + D10): transform → form flag → `mProcID`
  (`PROC_WAIT` v1) → `setMatrix()` → `allAnimePlay()` → `calc()` → **pose copy via
  `J3DModel::setAnmMtx`** (+ `setScaleFlag`, D10) + `setBaseTRMtx` → `setItemMatrix`/
  `setWolfItemMatrix` → `setBodyPartPos` → `setAttentionPos` → `setCollisionPos` →
  **per-frame Tg registration** (`dComIfG_Ccsp()->Set` ×3) → `mLinkAcch.CrrPos` → `setRoomInfo`.
- **Sender** (02 §3): post-execute read on the real Link: `current.pos`, `shape_angle.y`,
  `mBodyAngle.x`, `checkWolf()`, `getBaseTRMtx()`, per-joint `getAnmMtx` (~40), face
  `{bckIdx, btpIdx, frame}`, `stateFlags`, scale flags. Every frame, gated on a same-room remote.
- **Wire** (00-network §5 + R18): `PlayerState` raw matrices (~2.7 KB; stage name + roomNo),
  `PlayerEvent` reliable (form/equip/item-joints/horse-inline/attention).
- **Accept**: two fork builds on LAN; remote Link mirrors pose exactly (matrix-copy); frozen in
  cutscenes; survives room changes; no host-save damage; disable/reconnect clean.

### M2 — Enemy authority + targeting

- **Freeze (generic)**: in-source guard at the top of `fopAc_Execute` — skip AI for enemy
  puppets, keep draw; `old = current` after apply; `cullMtx` from `current.pos` (R10);
  per-whitelist-type profile check for `fopAcStts_CULL_e`.
- **Apply**: base fields + per-type adapter (reuse the fork's `EnemyAdapter`; add
  `refreshColliders`, `driveModel`, `dropTableId`, `semantics`). Whitelist start: `E_AI`, `E_HM`,
  `E_FB`, `E_DF`, `E_GS`, `E_YC`, `E_MD` + one `B_*` boss.
- **Targeting (D3 — the pivot's payoff)**: extend `fopAcM_getContextPlayer()`/`ScopedContext`
  from NPCs to enemy AI — the inline player resolution returns the *nearest real player* (host
  Link or a remote puppet) for enemy targeting reads. Enemies chase and attack remotes.
  Per-type verification that the context swap covers each whitelisted enemy's reads.
- **Combat** (03 §4): client intercept at `dCcS::SetAtTgGObjInf` → `CombatIntent` → owner
  validates → injects fabricated hit into the enemy's own damage handler → `CombatResult` +
  `EnemyEvent(died)`. Multi-hit batching per enemy per frame.
- **Drops (D5)**: on `EnemyEvent(died)`, each client explicitly spawns the drop
  (`fopAcM_createItemFromEnemyID` with the adapter's drop-table id) + death-anim/VFX beat before
  delete. Host needs no gating.
- **Room-clear**: dead-puppet deletion on `EnemyEvent(died)` keeps ALLDIE scans in step; reliable
  per-room `EnemyEvent(roomClear)` bit for corpse/dynamic edge cases.
- **Bosses (D9)**: v1 = boss exists iff the room owner's story needs it; on death,
  `EnemyEvent(died, flagMask)` → per-player local flag grant.
- **Accept**: host fights an enemy; client sees same HP; client's hits land and kill; enemies
  chase both players (D3); drops spawn on both; doors open together.

### M3 — Time & weather

Per 04, fork-trivial attachment: guard `setDaytime()` for clients; force weather pre-`exeKankyo`;
suppress the local dice machine (type-4, verifying R16); re-assert on stage change post-`dKy_Create`.
Wolf-howl rate handling is the fork's own code (R8 moot). Twilight per 04 §5.5. Daybreak
wolf-revert built in per-player `forms` watching DAWN/DUSK events (vanilla has none — verified).
**Accept**: same sky, rain together, cutscene freeze matches, stage transitions re-assert.

### M4 — Session polish + room ownership

> **Status: IMPLEMENTED (M4 complete; M4.5 fix pass COMPLETE — see below).**
> Every block below landed on `net-coop`; the selftest (318 checks incl. the
> M4 suites: room-ownership routing to a non-host owner, same-room
> snapshot/event scoping, ownership transfer, worldStage carry, entity-id
> stability, LAN discovery) is green on a forced rebuild.
>
> **M4.5 (review-m4-glm-5.2.md):** MAJOR 1 — join-warp REMOVED by user
> decision: a joining client stays in its own save stage (stay-put; the D6
> row + §5 M4 block below record the revised policy). KEPT: the `worldStage_`
> fill from the real Link + the stage carry in JoinAccept/WorldInit (the
> cross-stage `stageOk` puppet gate needs them). MAJOR 2 — `EnemyEvent`
> (died/room-clear) routed room-scoped like `EnemySnapshot`, plus a
> receive-side local-room gate (no cross-stage `(roomNo<<8)|setID` mis-kill /
> wrong drop / wrong save switch / ALLDIE corruption). MINOR 1 — the
> SceneChange room-table sniff keys only same-stage moves (the wire `scene`
> flag). MINOR 2 — dead code dropped. MINOR 4 — discovery log-line doc fix.
> MINOR 5 — wire-determinism sweep covers all 16 types. MINOR 6 — announce
> player count seeded before the first datagram (no 0-player announces).
> MINOR 8 — the join-warp statics went away with the feature. Evidence:
> forced clean rebuild green with zero warnings from the touched TUs;
> `dusk_net_selftest` 318/318 PASS. (Live loopback cannot be re-run here —
> the RVZ was removed from the tree.)

**Room ownership (from `docs/design/network.md` §6 — moved up from M5 by user decision):**

- Owner = first player in the room; the world host defaults to owning its own room. Ownership is
  **sticky**: transfers only on leave/disconnect, never on arrival (no ping-pong).
- The room owner sims that room's **enemies** (AI/HP/spawns) + enemy-caused world effects; every
  other player in the room sees them as snapshotted puppets (existing M2 freeze/apply machinery,
  now owner-scoped).
- **Combat intents route to the room's owner** (message destination — extend the RelayPolicy seam:
  `CombatIntent` no-relay + route-to-room-owner, `EnemySnapshot` owner→clients). Today intents go
  to the host (`SendToPeer(0)`); room ownership redirects to the owning peer.
- **Same-room scoping** for the enemy snapshot sender gate (M2 review MINOR-6: `RemoteInRoom` must
  scope by the remote's actual room, not "any present player").
- **Entity-id stability across ownership transfer**: map transfer, not renumber. Stage-placed
  `(roomNo, setID, procName)` keys survive; the dynamic-spawn counter is per-owner, so dynamic
  entities must be re-keyed or re-spawned on takeover (03 §3.1).
- **Owner takeover**: re-sim per the new owner's story (dead enemies may resurrect — accepted);
  the `roomClear` bit (already on the wire) keeps client ALLDIE scans honest.
- **Bosses** (D9): v1 = the ROOM owner's story decides boss existence; per-player flag grant on
  death unchanged.
- Test: two players in different rooms — each room's enemies sim on that room's owner; clients in
  the other room see them frozen/puppeted; combat from either side lands via the room-owner route.

**Session polish:**

- **Join policy (D6 — REVISED: join-warp removed by user decision)**: a joining client stays in
  its own stage; no forced teleports; players meet by traveling to a shared stage. KEEP the
  `worldStage_` fill from the real Link + the stage carry in JoinAccept/WorldInit — the
  cross-stage `stageOk` puppet-visibility gate depends on them (puppets stay hidden until both
  players share a stage). The `DecideJoinWarp`/`PerformJoinWarp`/leash machinery is removed.
- **Host leave UX (D8)**: freeze puppets + toast → return to single-player; decided here.
- Disconnect/leave: puppet despawn, slot kept for rejoin; ownership transfer on the disconnecting
  room owner.
- **LAN discovery**: `HostAnnounce` UDP broadcast (port 44771 reserved) + manual-IP config UI;
  wire the `net.*` config vars (registered, currently unread); `sessionActive()`/`hostRole()`
  retained for this.
- **Reload/disable**: session teardown per §3.6.

### M5 — Future

Dynamic waves (`EnemyEvent(spawn/die)`), horse entity channel, PvP (dummy damage table +
`DamagePlayer` intent), enemy pose matrix-copy upgrade, interpolation if internet play appears.
(Room ownership moved to M4.)

## 6. Risk register (Rev 3)

| # | Risk | Sev | Mitigation |
|---|------|-----|------------|
| 1 | Slot-0 pointer clobber during puppet create | High | Guarded restore in the create path; frame-boundary assert |
| 2 | Shared J3DModelData mtxCalc — Links render each other's anim | High | `modelCalc` per-Link mtxCalc re-assert |
| 3 | Host save corruption (restart-room, select-equip, damage→hearts) | High | Create-path guards + `setDamagePoint` guard |
| 4 | **Upstream merge burden** (new — the fork's price) | Med | Rebase discipline, `#if TARGET_PC` hygiene, small guarded diffs |
| 5 | **Context-swap targeting gaps** (new — D3) | Med | Per-whitelisted-type verification of covered reads; fallback = per-type action hooks |
| 6 | Puppet colliders not re-registered (freeze) | High | Per-frame Tg registration in the puppet path (D1) |
| 7 | Client drops never spawn | High | Explicit spawn on `EnemyEvent(died)` + death beat (D5) |
| 8 | Cross-stage join (no warp) | Low | Stay-put policy; `stageOk` hides puppets; players meet by traveling (D6 revised) |
| 9 | Room-clear desync (corpse linger / dynamic spawns) | Med | `EnemyEvent(roomClear)` bit |
| 10 | Frozen puppets in local cutscenes | Low | Accepted; optional pose polish |
| 11 | Joint-count/form mismatch (human 40 / wolf 37+) | Med | `jointCount` in packet; form atomic before pose |
| 12 | Frame-interp interaction | Med | Mandate `J3DModel::setAnmMtx` (interp-aware); never raw mtx buffer writes |
| 13 | Guard hygiene regressions (TARGET_PC drift) | Med | CI check: non-PC build stays vanilla (existing convention) |

## 7. Decisions — final record

All 12 decisions resolved (see Revision 3 table). Remaining open item: none at plan level; M4
decides the host-leave UX; M2 verifies context-swap coverage per whitelisted type.

## 8. Reference index

- `docs/design/network.md` — design decisions (authority, scope, per-triggerer story)
- `docs/design/mod-coop/00-network.md` — transport/session/protocol (amended: everything 60 Hz)
- `docs/design/mod-coop/01-puppet-link.md`…`04-time-weather.md` — mechanics (attachment framing = guards)
- `docs/design/mod-coop/review-kimi-k3.md`, `review-glm-5.2.md` — adversarial validation

## 9. M1 review resolutions (adversarial pass)

Adversarial reviews of the M1 player-replication milestone
(`review-m1-deepseek-v4-flash.md`, `review-m1-glm-5.2.md`) found one build
blocker, one create-path correctness blocker, and four majors. All are resolved
in the `net-coop` follow-up (M1.5). Commit mapping below.

| Finding | Fix (commit) |
|---------|--------------|
| **A1** HEAD unbuildable at the upstream aurora: the game stub `AIInitDMA(u32,u32)` never satisfied JASAiCtrl's `_AIInitDMA` reference (the pinned fork had masked it). | Stub now matches `AIInitDMA(uintptr_t, u32)` (C linkage via `dolphin/ai.h`) → emits `_AIInitDMA`. Verified with a forced rebuild (touched the stub + `JASAiCtrl.cpp`, deleted+rebuilt `libaurora_os.a` against `6c4c27f9`): the full game links and boots. aurora stays on the upstream pointer. (`1d80fded5d fix: AIInitDMA stub linkage`) |
| **B1** `setStartProcInit()` ran in full for puppets (host horse repositioned/force-ridden, story procs/demos started). | Puppets now call `procWaitInit()` **instead** of `setStartProcInit()` (`midna_prm = 0; procWaitInit();` in the puppet branch). The phase-2 wait terms that read the host save/global entry mode (`isHorseStart`, `checkCanoeStart()`, `checkBoarStart()`, `startPoint == -4` portal search) are suppressed for puppets so a puppet create cannot spin on, or bind to, host ride actors/portal. (`0b7f8bb5d1 alink: skip setStartProcInit for puppets ...`) |
| **M1** roster never rebroadcast on join → 3rd+ players invisible to earlier joiners. | On a successful join the host now `SendToAll(WorldInit, current roster, exceptPlayer=newId)`; `WorldInit` is documented as a roster-refresh. Selftest extended (join A, join B; assert A's roster shows B and B's shows A). (`5d2cbe6be0 net: broadcast roster on join`) |
| **M2** mutual room-change sender-gate deadlock (both puppets held hidden forever). | `PlayerEvent(SceneChange)` is now sent regardless of the sender gate (moved ahead of it in `sendPlayerState`), and the gate opens when our room changed since the remote's last state (`ReceiveSlot::myRoomAtLastRecv`), so one packet un-sticks both sides. (`74d5a1dce2`) |
| **M3** a create stuck on `cPhs_INIT_e` locked `g_createInFlight` forever. | Create-phase deadline (`kCreateDeadlineFrames = 600`): a puppet whose create exceeds it is deleted (destructor clears the entry + lock); after 3 strikes the spawn is dropped until the player leaves or a create succeeds. (`74d5a1dce2`) |
| **M4** `enet_deinitialize` ran before the coop teardown that sends SessionEnd/PlayerLeave. | `dusk::config::shutdown()` now calls `dusk::coop::shutdown()` **before** `dusk::net::shutdown()` and the misleading comment is fixed. (`46409fba68 net: shutdown order`) |
| **Doc drift** 00-network.md §5 joint-list layout, §6 ~2.7 KB. | §5 rewritten to the v3 raw-matrix `PlayerStateMsg`/`PlayerEventMsg` (2017 B, 48-B 3x4 Mtx; state-flags table; event table). §6 corrected to ~2.0 KB/player/frame → ~1.0 MB/s worst case. `WorldInit` documented as a roster-refresh broadcast. (`647a91f49c`; the 2017 figure includes the 16-B stage field — capstone MINOR I) |
| **`seq`** (02-player-state.md §2 `seq u16`) never shipped. | Reconciled the docs: `seq` dropped from §2; M1 stale policy is "stale = last pose held, no fade" (seq-gap timeout deferred to a future internet milestone). `ReceiveSlot::lastFrame` dead-coded. (`647a91f49c`) |
| **Minor a/b/c/d/e/f** | (a) wire-size comment 2657→2017 computed from `sizeof(Mtx)` (the 16-B stage field included — capstone MINOR I); (b) `modelCalc` mtxCalc re-assert gated on `remoteCount() > 0 || isPuppet(this)` so single-player with net off is byte-for-byte vanilla; (c) face/hat re-assert deferred with a playtest-verify note (their calc is driven by direct `setAnmMtx` + body base, not the anm-blend calc); (d) `RelayPolicy(type)` seam in `HandleData`/`ForwardGameMessage` (M2 CombatIntent must not relay to all; EnemySnapshot owner→clients only — seam only, not wired); (e) `lastFrame` removed per the `seq` decision; (f) `hostRole()`/`sessionActive()` documented as retained for the M4 host-leave/discovery UI. |
