# Networking design (mod-coop)

Status: **Design — authored by main session. Feeds the consolidated implementation plan.**

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
- **Authority: layered on top.** The sim owner of a room (v1: the world host in
  its own room; later: the designated room owner per `docs/design/network.md`
  §6) is the authority for that room's enemies and combat validation. Authority
  is expressed in the protocol as message *destination*, not a separate
  routing layer: enemy/combat messages flow to the sim owner; snapshots flow
  back from it.
- **Scene/room scoping** (Anchor model): a client only spawns/receives dummies
  and enemy state for players in its own scene. Clients in different scenes
  simply don't render each other; on scene change, the client requests the
  current state for its new scene.

## 3. Threading model

```
socket thread (mod-owned)          game thread (mod_update + hooks)
  enet_host_service()                drain inbox  → apply state
  recv → inbox ring                  build snapshots (per-frame clock)
  outbox ring → enet_peer_send()     enqueue outbound → outbox ring
```

- Mod spawns the socket thread; ENet runs exclusively on it. Same shape as
  Anchor's network-thread queue / game-thread processing split.
- Cross-thread handoff via fixed-size SPSC ring buffers (no allocs in the hot
  path). Frames are batched at the frame boundary, so contention is ~zero.
- `mod_shutdown`: set stop flag → join thread → `enet_host_destroy`.

## 4. Session lifecycle

| Step | Message | Channel | Notes |
|------|---------|---------|-------|
| Host up | `HostAnnounce` (UDP broadcast, port 44771) | — | game id, session name, players 0/8 |
| Discover | (listen for announce) | — | also manual IP join via config var |
| Connect | ENet handshake | — | ENet-level |
| Join | `JoinRequest` | reliable | name, client version, requested slot |
| Accept | `JoinAccept` | reliable | assigned `PlayerId` (session-wide 0–7), roster, stage, time, weather |
| Reject | `JoinReject` | reliable | reason (full, version mismatch) |
| Mid-game | `WorldInit` | reliable | stage name, room, spawn anchor, full roster, all current player/enemy states |
| Leave | `PlayerLeave` | reliable | host relays; puppet despawns |

- **PlayerId mapping**: host assigns session-wide ids 0–7 (`MAX_LOCAL_PLAYERS`).
  Each machine's `Runtime` keeps its local slot; remote slots are puppet-only
  (see `docs/design/network.md` §3).
- **Join mid-game**: client warps to the host's stage/room via a forced stage
  change, then receives `WorldInit` with the full current state.
- **Host departure**: v1 ends the session with a `SessionEnd`; no host
  migration (listed as future work).

## 5. Message protocol

All messages: `u16 type` + `u16 size` + payload. Fixed-size little-endian
structs, hand-written serializers (no reflection, no heap in the hot path).
Packet version in `JoinRequest`; mismatches rejected.

| Message | Channel | Cadence | Payload |
|---------|---------|---------|---------|
| `JoinRequest` / `JoinAccept` / `JoinReject` / `PlayerLeave` / `SessionEnd` | reliable | event | see §4 |
| `WorldInit` | reliable | on join | stage, room, spawn, roster, full state snapshot |
| `PlayerState` (per remote player, same scene) | unreliable seq | every frame | see PlayerState below |
| `PlayerEvent` (form change, mount/dismount, respawn, scene change) | reliable | on change | player id + event + data |
| `EnemySnapshot` (per enemy) | unreliable seq | every frame | see EnemyState below |
| `EnemyEvent` (spawn, die, room-clear, boss phase) | reliable | on change | enemy id + event + data |
| `CombatIntent` | reliable | on attack | attacker, target enemy id, attack kind, position |
| `CombatResult` | reliable | host→all | target enemy id, damage, new HP, outcome |
| `TimeSync` | unreliable seq | 1 Hz | time phase (u32) + elapsed delta |
| `TimeEvent` (new day, dusk/dawn) | reliable | on change | event id + time |
| `WeatherChange` | reliable | on change | weather id + intensity |

### PlayerState (per remote player, per frame)

Modeled on Anchor's `PLAYER_UPDATE`: **full pose, not animation state**.
Final joint count/read strategy comes from investigation 02.

```
playerId     u8
scene        u8              // only same-scene peers receive/apply
pos          f32 x3          // cXyz
rot          s16 x3          // shape rotation
joints       Vec3s[N]        // full joint pose (Anchor: 24; TP TBD)
upperLimbRot Vec3s
movementFlags u8
form         u8              // human / wolf (Anchor's modelGroup analog)
cosmetics    u8 x3           // tunic/shield/boots (TP equivalents)
stateFlags   u32             // subset relevant to rendering/combat pose
itemAction   s8              // held item action, for model group
invincibility u8
~= 150–300 bytes/player/frame (joint-dominated)
```

### EnemyState (per enemy, per frame)

Provisional — final schema from investigation 03:
```
enemyId   u16             // session-unique per room instance
type      u16             // procName / profile id
pos       f32 x3
angle     s16
hp / maxHp u16/u16
anim      u32             // action/anim state hint (see §7 note)
aggro     u8              // target player id (0xFF = none)
flags     u8              // frozen, dead, boss-phase…
~= 40–60 bytes/enemy/frame
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
  reliable.

### Bandwidth

- 8 players × ~2.7 KB (raw matrix pose) + 40 enemies × ~50 B, all at 60 Hz ≈
  **~1.4 MB/s worst case**. Trivial on LAN; Anchor does more (JSON) over the
  internet. ENet's `enet_host_bandwidth_limit` caps it if ever needed.

## 7. Enemy authority & combat

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
- `TimeSync` (phase + delta, 1 Hz, unreliable) keeps clients advancing; a lost
  packet self-corrects on the next one (absolute phase included).
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

- Config vars (via `svc_config`): `host_port`, `join_host` (manual IP),
  `session_name`, `enabled`.
- Host/discovery UI: minimal v1 — host starts on a config var or a menu
  binding; LAN discovery via broadcast announce + `svc_log` debug output.
- Debug: LogService for session/snapshot stats; a `--net-stats` style toggle
  prints per-frame bytes + snapshot rates.

## 11. Open integration points (to resolve with investigations)

1. **Forced stage change on join** — client must warp to host stage/room;
   calling `dStage_changeScene` from a mod (public) or a warp hook — confirm
   with investigation 01.
2. **Spawn anchor** for mid-game joins — host picks a safe anchor (near host
   player or a warp point); needs stage/room data from investigations 01/03.
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
