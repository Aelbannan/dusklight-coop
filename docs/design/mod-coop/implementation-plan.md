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
| D6 | Join warp | **Unlock-gated** — refuse/pin safe anchor if the client's save hasn't reached the host's stage |
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

### M4 — Session polish

- **Join warp policy (D6)**: unlock-gated warp + safe-anchor fallback; `dStage_playerInit`
  assert/timelayer interaction documented; acceptance test for un-reached stages.
- **Host leave UX (D8)**: freeze puppets + toast → return to single-player; decided here.
- Disconnect/leave: puppet despawn, slot kept for rejoin; LAN discovery + manual IP config UI.

### M5 — Future

Room ownership (sticky transfer), dynamic waves (`EnemyEvent(spawn/die)`), horse entity channel,
PvP (dummy damage table + `DamagePlayer` intent), enemy pose matrix-copy upgrade, interpolation
if internet play appears.

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
| 8 | Join warp vs save progression | High | Unlock gate + safe anchor (D6) |
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
