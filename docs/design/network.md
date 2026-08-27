# Design: Networked co-op (LAN-first)

Status: **Historical — design record of the pre-M5.1 hybrid (shared enemies,
shared sky). Not the live model.**

Live co-op (protocol v10) is **parallel worlds**: each machine sims its own
enemies, clock, and sky. Only player puppets cross the wire. Stay-put join:
players meet by traveling to a shared stage and room.

This document records the architecture decisions discussed before that pivot.

## 1. Problem statement

Local split-screen co-op runs into a wall: the game's story assumes a single
protagonist. Cutscenes, demos, NPC conversations, and route-blocking events all
key off *one* Link (`d_a_alink_demo.inc`, `dSv_info_c` flags, the party-sync
event machinery in `coop_event.h`). We keep fighting "story authority."

Proposed direction: move the multiplayer core online and change who owns what.

## 2. Decisions (the hybrid)

| Concern | Owner | Sync? |
|---------|-------|-------|
| **Story progress** | Local (each player's own save) | **No** — never crosses the wire |
| **World state** (chests, switches, doors, item pickups) | Local (per-player instances) | **No** — "trigger it yourself" |
| **Enemies** (AI, HP, spawns) | Room owner (see §6) | **Yes** — host/room-owner authoritative |
| **Players** (Link state) | Client-owned Link | **Yes** — replicated as puppets |
| **Time of day / weather** | World host | **Yes** — cheap, synced |
| **Cutscenes / demos** | Local, strictly per-triggerer | **No** — no party gather, desync is the design |

The one-line model: **each player is the protagonist of their own game, inside
one shared world** (shared geometry, shared enemies, one sky).

## 3. Architecture

```
HOST — the existing local mod, one process
  ├ full sim: geometry, room logic, physics
  ├ enemies in the host's room: authoritative (AI, HP, spawns)
  ├ host's Link: locally simulated (as today)
  └ remote Links: puppets (state from clients)

CLIENT — a full TP sim of its own
  ├ own Link: fully local — own save, own story, own demos
  │   (protagonist; zero sync)                       ← save:: subsystem
  ├ world actors (chests, switches, doors, NPCs):
  │   local, per-player — nothing to sync
  ├ enemies in rooms nobody else owns: locally simmed
  ├ enemies in owned rooms: puppets (local AI suppressed)
  └ other players: puppets

Transport: star (all relay through the world host).
Authority: distributed per room (see §6).
```

Both sides share one new piece of code: the **puppet** — a `daAlink_c` (or
enemy actor) driven by received state instead of local input/AI.

### Runtime in the network world

Today `Runtime` holds all 8 `PlayerId` slots in-process (split-screen). In the
network world, `PlayerId` becomes a **session-wide id (0–7)**, and each machine
runs its own `Runtime` with:

- its own player slot active (locally simulated), and
- remote player state carried in replicated slot data, not a second
  in-process `daAlink_c` *simulation*.

Same-machine split-screen can still run multiple local slots; networked slots
are puppet-only.

## 4. Sync channels

| Channel | Direction | Cadence | Payload |
|---------|-----------|---------|---------|
| Enemy snapshots | room owner → all | 20–30 Hz, delta | transform, action/anim state, health, phase, aggro target |
| Player state | each client → host → all | 20–30 Hz | Link transform, action state, form, health, active-item |
| Combat intents | client → room owner | on event | attack/effect validation requests |
| Combat results | room owner → all | on event | damage applied, deaths, drops |
| Room entry | client → room owner | on transition | "needs encounter" bits (§7) |
| Time | world host → all | ~1 Hz + events | phase + elapsed delta, new-day/dusk/dawn |
| Weather | world host → all | on change | rain/storm on-off |

Bandwidth is a non-issue: TP rooms hold a few dozen actors; 2–8 players at
30 Hz delta-compressed is trivial on LAN (and fine on broadband).

## 5. What is deliberately NOT synced

- **`dSv_info_c`** (save flags, event flags, dungeon bits) — never crosses the
  wire. Each machine owns its save; the existing per-player companion save
  (`save::`) is the right abstraction, pointed at a local file per machine.
- **Chests / switches / doors / pots / rupees / item pickups** — per-player
  client-local instances. Duplicate pickups are a *feature* (both players get
  the heart piece).
- **Cutscenes / demos / NPC dialogue** — play locally for whoever triggered
  them, starring that player's Link. No replication.
- **Camera** — fully local (the per-view camera machinery already exists).

### Enemy-caused world changes leak anyway

A boss breaking a wall, a door opening on room-clear, scripted enemy events —
these are world changes *caused by shared actors*, so they ride the enemy
channel. Expect the "no world sync" line to be fuzzy here; that is accepted.

## 6. Room ownership (per-room host)

Players may be in different rooms. Authority is distributed:

- **Owner** = first player in the room (world host defaults to owning their own
  room). Ownership is **sticky**: transfers only on leave/disconnect, never on
  arrival — no ping-pong.
- The room owner sims that room's **enemies** (AI/HP/spawns) and enemy-caused
  world effects. All other players in the room see them as snapshotted puppets.
- **Combat intents** against a room's enemies validate against that room's
  owner. **Friendly fire** resolves wherever both puppets are (world host
  relays).
- On takeover after a disconnect, re-sim per the new owner's story (dead
  enemies may resurrect — acceptable). A minimal room-state snapshot is a later
  refinement.
- Room boundaries already exist in code: the `mRoomNo`-keyed zone system in
  `include/d/d_save.h` and the `enemy::` room-clear tracking.

## 7. Story-gated enemies (bosses)

A boss is **just an enemy in a room**, simmed by the room owner. Two small
additions:

- **Existence**: the room owner spawns it if *any present player's story needs
  it* — a single "needs encounter X" bit per player, sent on room entry. This is
  not story sync; it is a tiny request bit. It lets an already-cleared player
  help a friend fight "their" boss.
- **Death**: grant the completion flag to every present player whose story
  expects it. No killing-blow tracking. Co-op boss kills just work.

Fallback v1 if the union bit is unwanted: boss exists iff the room owner's own
story needs it; co-players may help but may not get the flag. Same code path.

### Why not "local behavior, shared health"?

Local-AI boss instances in the same room diverge instantly (different targets,
different attacks) — you would see two bosses drifting apart. Room ownership
already gives shared health *by construction* (one instance), so the only
"local behavior" case is a solo fight in your own room, which is the normal
client-local sim. No extra machinery needed.

## 8. Story events — strictly per-triggerer

Story events are **strictly per-triggerer**: whoever triggers an event runs it
locally, starring their own Link, immediately. No waiting, no barriers.

The party-gather machinery in `coop_event.h` (`WaitingForParty` state,
`readinessMask()`/`readyCount()`, gather toasts, the
`deferStageEntry`/`deferStageExit`/`deferPartyStory` arbiter flow) is
**removed** for story events. The remaining scopes map as follows:

| Existing scope | Network mapping |
|----------------|-----------------|
| `InitiatorOwned` (Conversation) | As-is — initiator presents locally, rewards applied to their local save, minus the multi-player reward fan-out. |
| `PartySynchronized` (PartyStory) | Removed — treated as initiator-owned local presentation. |
| `P1Story` (StageEntry/StageExit) | Per-player local — each player's stage transitions are their own. |

Room-clear gating (`enemy::roomClearBlocked()`, "Gather Up" toasts) is a
*separate* system from story gathering; under per-room ownership (§6) its state
belongs to the room owner and needs its own decision — revisit separately.

## 9. Time and weather

- One clock, one sky: **world host** owns time-of-day + weather (global across
  rooms).
- Time: phase + elapsed delta at ~1 Hz, plus explicit events (new day,
  dusk/dawn transitions) — it is a clock, deltas suffice.
- Weather: sync on change only. Hook points: `src/d/d_kankyo.cpp`,
  `src/d/d_kankyo_rain.cpp`, `src/d/d_kankyo_wether.cpp`.
- The daybreak wolf-revert mechanic composes with the per-player `forms`
  subsystem: each player's form applies the shared time locally.

## 10. Edge cases & policies

- **Concurrent triggers of the same story event**: local saves make this a
  non-issue (each save dedupes via its own flags).
- **Two players at different story points in the same dungeon**: the union
  bit (§7) keeps the boss present for both; only the player who needs it gets
  the flag on death.
- **Disconnect**: room ownership transfers (sticky rule, §6); the dead player's
  puppet despawns; their local story resumes when they rejoin.
- **Join mid-game**: new client boots their own save; they enter the world as a
  local Link at a valid spawn point; room owners start sending them room state.

## 11. Build order

1. **Enemy snapshot + puppet `daAlink_c`** on the existing local sim — the one
   piece of genuinely new code both sides share.
2. **Star transport** (connection, serialization, session PlayerId mapping).
3. **Combat intent validation** against the sim that owns the target.
4. **Room ownership** (authority distribution, transfer rules).
5. **Boss policy** (union existence bit, death flag grant) — authority/policy on
   top of the enemy plumbing, not new plumbing.
6. **Time/weather sync** — small, independent, cheap wins.

## 12. Explicit non-goals (for now)

- Deterministic lockstep (native code is non-deterministic; snapshots only).
- Internet-grade latency handling (input prediction / rollback). LAN-first;
  revisit only if internet play is wanted and mushy controls prove unacceptable.
- General world-state replication layer. Per-player world state is free
  (client-local) and is the default.
