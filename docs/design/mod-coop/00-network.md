# Networking design (mod-coop)

Status: **Historical above the M5.1 cut; v10 wire is in `include/dusk/net/protocol.h`.**

Live co-op is **parallel worlds + player puppets** (protocol v10). Enemies,
clock, and sky stay local — they never cross the wire. The sections below that
describe host-authoritative enemies, combat validation, TimeSync, and room
ownership are the pre-pivot design record.

This is the transport/session/protocol design for the network layer of the co-op
mod. It is intentionally agnostic of the game-side integration details (those
come from the per-area investigations) — it defines the message surface the
game-side code hangs off.

## 0. Reference base: SoH Anchor

This design is modeled on **Anchor**, Ship of Harkinian's co-op netcode
(`soh/soh/Network/Anchor/` in HarbourMasters/Shipwright, also ported to 2S2H).
Anchor is the closest proven precedent: a decomp port running co-op with
per-frame player pose sync and dummy player actors. What we borrow from it:

- **Per-frame player updates with full-pose sync** — send every frame, apply
  directly, no interpolation (§6).
- **Pose, not animation state** — the source player's joint table is
  transmitted and pointer-swapped into the dummy player. Zero state-machine
  coupling (§5).
- **Dummy player pattern** — spawn a real Link actor, hijack it before its
  first update (ghost actor id, NPC category, custom update/draw), drive it
  purely from received state; real collision cylinders so it participates in
  combat (investigation 01 evaluates this against TP's engine).
- **Scene-scoped updates** — a client only renders dummies for players in the
  same scene/room, and only same-scene peers receive your updates (Anchor hides
  out-of-scene dummies at -9999).
- **Packet taxonomy & queue pattern** — handshake, state, event, and
  request/reply packet types; network-thread queue → game-thread processing.
- **Room/team state** — per-room authority (owner, PvP mode, sync options) and
  team membership as sync scopes.

What we deliberately change vs Anchor:

- **No world/quest sync.** Anchor's `SetFlag`/`GiveItem`/`UpdateDungeonItems`/
  `SetCheckStatus`/`EntranceDiscovered`/`GameComplete`/`UpdateRoomState`
  packets are the bulk of its surface — we do NOT do these. Story/inventory/
  world state stays per-player (each machine runs vanilla with its own save).
- **Host-authoritative enemies.** Anchor does not sync enemies at all — every
  client simulates its own instances. We extend with host-authoritative enemy
  state + combat validation (§7), which is the one genuinely new subsystem.
- **Transport.** Anchor uses JSON over a WebSocket to a dedicated Go server
  (internet-capable). We use ENet over LAN (§1): reliable+unreliable channels,
  binary serialization, no server process. Anchor's per-frame JSON is proof
  bandwidth is a non-issue either way.

## 1. Transport

- **ENet 1.3.x** (zlib license). Rationale: LAN-appropriate, tiny, channels with
  per-channel reliability/ordering built in, connection lifecycle for free.
- **Vendored as source** (`enet.c` + `enet.h` compiled into the mod), not a
  runtime library: avoids all `RUNTIME_LIBRARIES` path/ABI issues and keeps the
  build self-contained. `RUNTIME_LIBRARIES` remains the fallback if a bigger
  dependency is ever needed.
- **Channels**: `0` = reliable (control, events, combat), `1` = unreliable
  sequenced (snapshots). This avoids the head-of-line blocking Anchor gets from
  mixing everything over one reliable stream — irrelevant on LAN, matters if
  this ever goes over the internet.
- **Platforms**: desktop (Windows/macOS/Linux) v1. Mod loading on iOS/Android
  is unverified — do not block on it.

## 2. Topology and authority

- **Transport: star.** The world host (session creator) relays all player
  state. Every machine connects to the host; no P2P mesh in v1.
- **Authority: layered on top.** The sim owner of a room (M2: the world host
  in its own room; M4: the designated room owner per `docs/design/network.md`
  §6) is the authority for that room's enemies and combat validation. Since
  M4 the authority map is an explicit wire object: the host maintains a
  sticky per-room owner table (host-defaults-own-its-room, transfer on
  leave/disconnect) and broadcasts each assignment as `RoomOwnership`
  (reliable, host→all, plus the full map to each joiner). Authority is
  expressed in the protocol as message *destination*, not a separate routing
  layer: enemy/combat messages flow to the room's owner; snapshots flow back
  from it (host-relayed, room-scoped).
- **Scene/room scoping** (Anchor model): a client only spawns/receives dummies
  and enemy state for players in its own scene. Clients in different scenes
  simply don't render each other (puppets stay hidden until both players
  share a stage — the `stageOk` gate keys on the remote's REAL stage from
  `PlayerState`); players meet by traveling. M4/M4.5: enemy snapshots AND
  enemy events (died/room-clear) are additionally room-scoped — the sender
  gate checks each remote's actual room, and the host relays a client
  owner's snapshots/events only to peers in the sender's room. EnemyEvent
  room-scoping (M4.5 MAJOR 2) closes the cross-stage hazard: a died event
  from another stage with a coincident `(roomNo<<8)|setID` must not mis-kill
  a local enemy, spawn the wrong drop, grant a wrong save switch, or corrupt
  ALLDIE.

## 3. Threading model

```
socket thread (mod-owned)          game thread (mod_update + hooks)
  enet_host_service()                drain inbox  → apply state
  recv → inbox rings (rel+snap)      build snapshots (per-frame clock)
  outbox rings → enet_peer_send()    enqueue outbound → outbox rings
```

- Mod spawns the socket thread; ENet runs exclusively on it. Same shape as
  Anchor's network-thread queue / game-thread processing split.
- Cross-thread handoff via fixed-size SPSC ring buffers (no allocs in the hot
  path), **split by channel**: channel 0 (reliable control/events/combat) and
  channel 1 (unreliable snapshots) never share a ring. Reliable overflow is
  an explicit, session-visible failure — never a silent drop; snapshot
  overflow keeps the freshest entry (replace-newest) under pressure.
- `mod_shutdown`: set stop flag → join thread → `enet_host_destroy`.

## 4. Session lifecycle

| Step | Message | Channel | Notes |
|------|---------|---------|-------|
| Host up | (listen on hostPort) | — | Host/Connect is explicit; `net.autoConnect` only auto-starts a **client**. Join is by IP (`net.joinHost` + `net.hostPort`). |
| Discover | — | — | Removed. Manual IP join only. |
| Connect | ENet handshake | — | ENet-level |
| Join | `JoinRequest` | reliable | name, client version, requested slot |
| Accept | `JoinAccept` | reliable | assigned `PlayerId` (session-wide 0–7), roster, stage, time, weather |
| Reject | `JoinReject` | reliable | reason (full, version mismatch) |
| Mid-game | `WorldInit` | reliable | stage (name/room/layer/point) + time + weather (v5) + full roster — **fixed size**; current player/enemy state arrives right after as a burst of ordinary per-frame `PlayerState`/`EnemySnapshot` messages. **Doubles as a roster-refresh**: on every successful join the host re-broadcasts `WorldInit` (current roster) to all already-joined peers so earlier joiners learn about later players (M1 fix; without it 3+ players were invisible to earlier joiners) |
| Leave | `PlayerLeave` | reliable | host relays; puppet despawns; the leaver's rooms transfer ownership (M4) |

- **PlayerId mapping**: host assigns session-wide ids 0–7 (`MAX_LOCAL_PLAYERS`).
  Each machine's `Runtime` keeps its local slot; remote slots are puppet-only
  (see `docs/design/network.md` §3).
- **Join mid-game**: **M4.5 — the join-warp is REMOVED (user decision) —
  stay-put policy**: the client boots into its OWN save stage and stays
  there; no forced stage change. Players meet by traveling to a shared
  stage, where their puppets become visible to each other. On join the
  client receives `WorldInit` (host stage + roster), followed by the current
  per-player/per-enemy state as a burst of ordinary `PlayerState`/
  `EnemySnapshot` messages on the per-frame stream (no separate full-state
  message — the first pose arrives within one frame). The host's stage in
  `WorldInit`/`JoinAccept` is the joiner's "where is the host" reference
  (the `worldStage_` carry from the real Link) — it does NOT trigger a warp.
- **Host departure**: M4 ends the session with a `SessionEnd`; the client
  toasts "Host left/disconnected" (per `SessionEnd` reason vs connection
  loss), despawns puppets, and continues as vanilla single-player. No host
  migration (listed as future work).

## 5. Message protocol

> **SUPERSEDED (v9):** the message set below is the DEAD pre-pivot v6 model.
> M5.1 shredded combat/ownership (`CombatIntent`/`CombatResult`/
> `RoomOwnership` deleted, `EnemySnapshot` renamed then dropped). v9 also
> dropped time/weather and ghost spectating. Type count is 8; `protocol.h`
> is the normative set. §7's combat-routing diagram is DROPPED. The
> envelope/serializer mechanism is unchanged, but the table below is
> history.

All messages: `u16 type` + `u16 size` + payload. Fixed-size little-endian
structs, hand-written serializers (no reflection, no heap in the hot path).
Packet version in `JoinRequest`; mismatches rejected.

| Message | Channel | Cadence | Payload |
|---------|---------|---------|---------|
| `JoinRequest` / `JoinAccept` / `JoinReject` / `PlayerLeave` / `SessionEnd` | reliable | event | see §4 |
| `WorldInit` | reliable | on join **+ re-broadcast as roster-refresh on every later join** | stage (name/room/layer/point) + time + weather (v5) + full roster — no other state sections; snapshot-on-join rides the per-frame stream (burst of `PlayerState`/`EnemySnapshot` right after) |
| `PlayerState` (per remote player, same scene) | unreliable seq | every frame | see PlayerState below |
| `PlayerEvent` (form change, mount/dismount, respawn, scene change, equip, attention) | reliable | on change | player id + event + scene + data + data2 (see PlayerEvent below; M4.5: for `SceneChange`, `scene`=1 marks a same-stage move — the host's room-table sniff keys (last-known stage, new room), valid for same-stage moves only) |
| `EnemySnapshot` (per enemy) | unreliable seq | every frame | see EnemyState below |
| `EnemyEvent` (spawn, die, room-clear, boss phase) | reliable | on change | enemy id + event + data — **M4.5: room-scoped** (a died/room-clear event reaches only peers in the SENDER's room; the receive side also gates on the local room) |
| `CombatIntent` | reliable | on attack | attacker, target enemy id, attack kind, position |
| `CombatResult` | reliable | room owner→all | target enemy id, damage, new HP, outcome |
| `TimeSync` | unreliable seq | **1 Hz** (real — M3.5: a 1 Hz `NetClock` gate; immediate on stage/rate change) | absolute phase `f32` 0..360 + day + rate + flags (v5; `protocol.h` `TimeSyncMsg` is normative) |
| `TimeEvent` (new day, dusk/dawn) | reliable | on change | event id + time + day (v5) |
| `WeatherChange` | reliable | on change | mode + thunder + intensity + colpat (v5) |
| `RoomOwnership` | reliable | on change + to each joiner | stage + room + owner PlayerId (v6; `RoomOwnershipMsg` is normative) — the M4 authority map |

Field order in the struct blocks below **is the wire order** (version-gated by
`kProtocolVersion`); the hand-written serializers in `src/dusk/net/protocol.cpp`
are normative, and `include/dusk/net/protocol.h` is the authoritative struct
reference (M1: rewritten to the Rev 3 D4 raw-matrix pose — the joint-list
layout below is history).

### PlayerState (per remote player, per frame)

Modeled on Anchor's `PLAYER_UPDATE`: **full pose, not animation state**. The
sender's per-joint `mAnmMtx` table is copied verbatim so a puppet renders the
exact blended/callback-baked pose with zero animation-state coupling
(investigation 02 §1.3). `protocol.h` is normative.

```
playerId      u8
roomNo        s8              // current.roomNo — same-scene/room scoping
stage         char[16]        // current stage (e.g. "F_SP103") — same-scene
                              // scoping: room numbers are not unique across
                              // stages (spring / house interiors)
form          u8              // 0 human / 1 wolf (checkWolf())
stateFlags    u8              // kPlayerStateFlag_* bits (table below)
jointCount    u8              // 0..kMaxJoints; joints[jointCount..] wire-zeroed
scaleFlags    u8[5]           // one bit per joint (setScaleFlag)
yaw           s16             // shape_angle.y
pitch         s16             // mBodyAngle.x
faceBckIdx    u16             // mFaceBckHeap.getIdx()
faceBtpIdx    u16             // mFaceBtpHeap.getIdx()
faceFrame     s16             // face frame ctrl frame
reserved      u8
pos           f32 x3          // current.pos
baseTR        Mtx (3x4)       // mpLinkModel->getBaseTRMtx() — exact world placement
joints        Mtx[40] (3x4)   // per-joint getAnmMtx(j), root-relative
= 2017 bytes/player/frame (5 + 16 stage + 5 scaleFlags + 10 + 1 + 12 + 48 +
40×48; Mtx is f32[3][4] = 48 B — capstone MINOR I corrected the stale 2001,
which omitted the 16-byte stage field)
```

`jointCount > kMaxJoints (40)` is rejected at parse (semantic validation,
M0.5) so apply code can never index `joints[jointCount]` out of bounds.

`stateFlags` byte (02-player-state.md §2.1):

| bit | meaning | source |
|-----|---------|--------|
| 0 | riding (any RIDETYPE) | `mRideStatus != 0` |
| 1 | invulnerable / damage-blink | `mDamageTimer > 0` |
| 2 | subjectivity (first-person) | `mProcID == PROC_SUBJECTIVITY` |
| 3 | downed/dead (local life state) | coop-local `PlayerLifeState` |
| 4 | demo in progress | `mDemo.getDemoType() != 0` |
| 5 | player no-draw (**never set on the wire**) | `checkPlayerNoDraw()` |

### PlayerEvent (reliable, on change)

```
playerId  u8
eventId   u8              // PlayerEventId
scene     u8
reserved  u8
data      u32             // event-specific (see below)
data2     u32             // extended payload (M1: item joints)
stage     char[16]        // v10: SceneChange destination; empty otherwise
```

| event | data | data2 | stage |
|-------|------|-------|-------|
| `FormChange` | form (0 human / 1 wolf) | — | empty |
| `SceneChange` | roomNo | — | destination stage name |
| `Equip` | equipItem u16 \| selectItemId u8 \| clothes u8 | leftItemJnt u16 \| rightItemJnt u16 | empty |
| `AttentionChange` | session entity id of the lock target (0xFFFF = none/local-only) | — | empty |
| `Mount` / `Dismount` / `Respawn` | (reserved; horse entity channel is M6) | — | empty |


### EnemyState (per enemy, per frame)

Provisional — final schema from investigation 03. Field order matches the
serializer (`src/dusk/net/protocol.cpp`).

```
enemyId   u16             // session-unique per room instance
type      u16             // procName / profile id
hp        u16
maxHp     u16
aggro     u8              // target player id (0xFF = none)
flags     u8              // frozen, dead, boss-phase…
angle     s16
anim      u32             // action/anim state hint (see §7 note)
pos       f32 x3
speed     f32 x3          // velocity (knockback / anim hints)
semantics u8              // per-type damage semantics tag (hp|hitCount|special)
reserved  u8[3]
= 44 bytes/enemy/frame (2+2+2+2+1+1+2+4+12+12+1+3 — the v4 layout;
capstone MINOR I corrected the stale ~28 B provisional)
```

## 6. Cadence — every frame, no interpolation

- **Everything sends every frame** (game frame rate, capped at 60 Hz) — players **and**
  enemies. Decided in review (D2); supersedes the earlier 30 Hz enemy draft in 03-enemies.md.
  Rationale: with pose-driven puppets, the latest packet IS the state — direct
  apply, no interpolation, lowest possible latency. Anchor sends every frame as JSON over the
  internet and works; a binary format on LAN is trivial.
- The game loop is the clock: the sender hook runs once per frame; build and
  enqueue snapshots there. No timer, no batching logic.
- **No interpolation in v1** — direct apply of the latest received state (Anchor model).
  Re-applying an unchanged enemy snapshot on intermediate frames is stated behavior; per-enemy
  dirty flags suppress unchanged enemies anyway.
- Time: `TimeSync` at 1 Hz (it's a clock — absolute phase self-corrects), events
  reliable. The 1 Hz cadence is real since M3.5: a `NetClock::AtRate(1)` gate in
  the host publisher (it was mistakenly driven by the 60 Hz snapshot clock in
  M3, sending 60×/s); stage changes and rate transitions still send
  immediately.

### Bandwidth

- Star topology fan-out, not a single 8-player stream: the host relays every
  PlayerState to every other joined peer **and** sends its own pose to each.
  Host outbound pose traffic is `(n-1)²` packets/frame. At 8 players that is
  **49 × 2017 × 60 ≈ 5.9 MB/s** payload before events — about 6× the
  naive `8 × 2017 × 60 ≈ 0.97 MB/s` figure. Two-player LAN is ~120 KB/s and
  is fine. v1 play should stay at 2–4 players on Wi-Fi; the 8-player id space
  is not a bandwidth budget. ENet's `enet_host_bandwidth_limit` caps it
  if ever needed. (Capstone MINOR I corrected the 2001-B/28-B figures; the ~2.7
  KB/4x4-Mtx figure was corrected back in review m1 M3. v7 dropped the 40×44 B
  enemy snapshot stream. v10 PlayerEvent is 28 B with the stage name.)

## 7. Enemy authority & combat

> **DROPPED (M5.1).** This section described the owner-authoritative combat
> model the pivot deleted. Co-op uses parallel worlds: enemies are local +
> vanilla, no combat crosses the wire, and ghost spectating was scrapped
> (never built). The diagram below is history.

```
client (attacker)                  host / sim owner                  all clients
  local Link hits enemy X
  → intercept damage path
  → CombatIntent(X, attack) ──────► validate (exists, range, state)
                                    apply damage to host sim
                                    → EnemySnapshot delta (HP) ─────► render
                                    on kill: EnemyEvent(died)
      ←──────────────────────────── CombatResult (optional ack)
  (no local damage application)
```

- **Enemy pose vs state**: investigate whether enemies can use the same
  pose-sync trick as players (full pose per enemy type) or need action/anim
  state. Anchor avoids this question entirely by not syncing enemies — this is
  our one novel subsystem, scoped by investigation 03.
- Drops: on `EnemyEvent(died)` each client spawns its own drop locally —
  pickups stay per-player (per `docs/design/network.md` §5).
- Friendly fire: v1 **off** — `CombatIntent` with a player target is rejected
  by the sim owner. Anchor's PvP machinery (dummy damage table + `DamagePlayer`
  packet) is the reference if enabled later.

## 8. Time & weather sync

- Host owns the clock and the sky.
- `TimeSync` (absolute phase `f32` 0..360 + day + rate + flags, 1 Hz,
  unreliable) keeps clients advancing; a lost packet self-corrects on the next
  one (absolute phase included).
- `TimeEvent` (new day, dusk/dawn) and `WeatherChange` are reliable events.
- Client application: force the time value + weather state each frame via the
  hook points from investigation 04; side effects (lighting, night-only
  spawns) come along for free because the engine reads the same state.

## 9. Host simulation of enemies (v1 co-location)

- v1: the world host is the sim owner for its own room (players co-locate,
  matching the existing "gather" playstyle). Client enemy AI is suppressed;
  clients render host-driven enemy puppets.
- v2: room ownership per `network.md` §6 — ownership transfers on
  leave/disconnect; combat intents route to the room's owner.
- Bosses: treated as enemies; the union existence bit + per-player death flag
  (network.md §7) are `EnemyEvent` policy handled by the sim owner.

## 10. Config, UI, and debug surface

- Config vars: `net.autoConnect` (persisted client autostart on launch),
  `net.connected` (launch override only; Host/Connect use in-memory intent),
  `net.role` (`"host"` / `"client"`, set by Host / Connect), `net.hostPort`,
  `net.joinHost` (IP or hostname; port is `net.hostPort`), `net.sessionName`.
  Legacy `net.enabled` is loaded to migrate: client+on → `autoConnect`, host+on
  does not auto-host; `--cvar net.enabled=true` still starts a session this run.
- UI: Settings → **Network** tab. Host / Connect / Disconnect. No LAN list.
- Debug: coop/net logs via the dusk logging system; no session = vanilla
  single-player.

## 11. Open integration points (to resolve with investigations)

1. ~~**Forced stage change on join**~~ — **RESOLVED (M4.5, user decision):
   the join-warp is REMOVED.** A joining client stays in its own save stage
   (stay-put); the net layer never calls `dStage_changeScene`.
2. ~~**Spawn anchor** for mid-game joins~~ — **RESOLVED (M4.5):** no warp
   means no anchor; players meet by traveling to a shared stage.
3. **Enemy id stability** — `enemyId` must stay stable across snapshots; assign
   at spawn on the sim owner, map through room-owner changes (investigation 03).
4. **TP pose surface** — joint count, read path, and pointer-swap vs per-joint
   copy feasibility for both players and enemies (investigations 01/02).
5. **Scene-change event** — clients must know when a peer changes scene to
   spawn/hide dummies (Anchor's sceneNum in every update handles this).

## 12. Non-goals (v1)

- No NAT traversal / internet play (LAN-first; host IP join as fallback).
- No host migration, no reconnection state save.
- No auth/cheat protection (trust model: LAN friends).
- No rollback/input prediction — clients render host state; LAN latency makes
  this acceptable.
- No shared world/quest state (flags, items, checks) — explicitly per-player.
