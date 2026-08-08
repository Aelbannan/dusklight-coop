# 05 — Ghosts: parallel-world co-op (the M5 pivot)

Status: **DESIGN (not yet approved)** — pending adversarial review.
Supersedes for M5+: the owner-authoritative enemy/combat direction of
`implementation-plan.md` M2/M3/M4, `m2-design-notes.md`, `03-enemies.md`,
and `00-network.md` §6 (room ownership). The transport/session/protocol
core (00-network.md §1–§5) and player/time/weather subsystems are unchanged.

## 1. The model — parallel worlds, ghost mirrors (user decision 2025)

**There is no shared enemy state, period.** Every machine simulates its own
world natively with vanilla AI:

- Each player's enemies/bosses are THEIR OWN copies. The room's Bokoblins
  exist independently on every machine in that room; hitting yours does not
  touch your friend's. Doors open per player when THEIR copies die (ALLDIE
  is local and needs zero sync). Two players in the same room each fight a
  parallel copy of the same fight, side by side.
- Cross-machine damage, assist combat, shared HP, drops routing, friendly
  fire, room-clear sync, boss-phase sync: **all gone**. Desync is accepted
  by design (each Link is the protagonist of their own game).
- The ONLY cross-machine interactions: player puppets (existing), time and
  weather (existing), and a new **ghost layer** — transparent mirrors of
  every remote player's enemies, bosses, and projectiles so you can watch
  each other fight. Ghosts are strictly visual (user decision): they cannot
  be hit, they deal no damage, and they are invisible to local AI.

This deletes the single largest source of story/world-authority bugs — the
whole M2/M4 authority stack — at the cost of surrendering "shared fight"
co-op entirely. v1 is *co-op play + spectate*, not *shared combat*. A
minimal assist path (fold your hits into your friend's sim) is a documented
M6 seam, deliberately not built now.

## 2. What dies (concrete, per file)

The following are **deleted or reverted to vanilla** in M5.1 — a shred,
not a refactor.

| Artifact | Where | Disposition |
|---|---|---|
| RoomOwnershipMsg + `RoomOwnershipTable` + `OwnerOf` + `UpdatePlayerRoom`/`OnPlayerEnter(Leave)` + transfer/join-map logic | protocol.h, session.cpp | **delete** |
| `SendToAllInRoom`/room-scoped fan-out + host `UpdatePlayerRoom` sniff on PlayerEvent(scene) | session.cpp | **delete** (ghosts are broadcast to all: `SendToAll`) |
| CombatIntentMsg, CombatResultMsg + routing, validation, synthetic `SetAtTgHitSynthetic` Tg-flag/synth collider injection, `applyInjectedHit` | protocol.h, coop_combat.cpp, coop_enemy.cpp, d_cc_s.cpp, d_cc_uty.cpp | **delete**; revert d_cc intercepts to vanilla `#else` paths or remove the hooks |
| Enemy freeze: `puppetExecute` pre-guard in f_op_actor.cpp, freeze/apply machinery, optimistic freeze (M4.6) | f_op_actor.cpp, coop_enemy.cpp | **delete** (nothing is ever frozen — every enemy is local) |
| Per-type combat adapters: `injectHit`, `isDead`, `deathSwitchNo`, `dropTableId`, `defaultPowerType`, `refreshColliders`, `DamageSemantics` | coop_enemy.cpp `kAdapters` | **delete** (a render-only ghost table replaces it, §5) |
| Drops gates + item distribution | coop_drops.cpp, coop.cpp routing | **delete** (drops are fully local per player's own kills) |
| Room-clear sync: `clientRoomCleared`/`hostRoomCleared` + d_a_alldie.cpp hook + `g_roomClear`/`g_roomHadEnemies` | d_a_alldie.cpp, coop_enemy.cpp | **delete** (ALLDIE is local; doors open per player) |
| D3 targeting: `ScopedEnemyTarget`, `currentTargetPlayer`, the f_op_actor_mng.h / d_a_player.h header-inline re-routing, E_YC/E_AI read routing | coop_context.cpp, f_op_actor_mng.h, d_a_player.h, d_a_e_yc.cpp, d_a_e_ai.cpp | **delete / revert to vanilla** (enemies always target their local slot-0 Link) |
| `EnemyEventMsg` fields: `data` (dropTableId), `flagMask` (switch grant), `BossPhase`, `RoomClear` events | protocol.h, coop_enemy.cpp | **trim** — Died survives with entityId + type only |
| `EnemySnapshotMsg` fields: `hp`/`maxHp`/`aggro`/`semantics` | protocol.h | **delete** (replaced by ghost pose, §4) |
| `registerDynamicEnemy`, dynamic id space | coop_enemy.cpp, coop_entity_logic.h | **keep dormant** (waves are M6; the owner-major id space is reused by ghost entity ids, §4) |

**What survives intact:** transport/session/discovery/config/shutdown,
PlayerState puppets + all `isPuppet` guards (create-path save guards,
invulnerability, hidden draw, weapon bones), forms, horses, time/weather,
the per-player save system, and the protocol envelope/serializer core.

## 3. Wire changes (kProtocolVersion → **7**)

v7 is a **breaking deletion** of the authority surface (§2), plus the new
ghost messages (§4). The version history comment in protocol.h gets the
v7 block. `PayloadUnion`, `WireSize`, `ChannelFor`, and the determinism
sweep are updated to match. `kMaxMessageSize = 4096` is unchanged
(GhostSnapshot is ~40 B).

## 4. Ghost layer

### 4.1 Identity — `GhostEntityId` (sender-major)

Every machine broadcasts the entities in **its own current room** (the old
`StageEntityId` = `(room<<8)|setID` for stage-placed enemies, owner-major
`(owner<<12)|counter` for dynamic spawns — the existing scheme in
`coop_entity_logic.h`). The receiver keys ghosts by **`(sender PlayerId,
entityId)`**, so a same-room pair's identical `(room<<8)|setID` never
collides: my E_AI#3 is a real actor, your E_AI#3 is a ghost keyed
`(you, 0x0303)` — double-vision by construction, no registry conflict.

### 4.2 Messages (fixed-size, hand-serialized — same contract as v6)

```cpp
struct GhostSnapshotMsg {          // unreliable (channel 1)
    u16 entityId;                  // (room<<8)|setID, or owner-major dynamic id
    u16 type;                      // procName (profile id)
    u8  flags;                     // boss bit, dead-gone bit, influence bits (v1: 0)
    s16 angle;                     // shape_angle.y
    u32 anim;                      // per-type packed: action id u16 | frame u16
    Vec3f pos;
    Vec3f speed;                   // knockback / projectiles
};                                 // 5+4+1+2+4+12+12 = 40 B

struct EnemyEventMsg {             // reliable (channel 0), trimmed
    u16 entityId;
    u16 type;
    u8  eventId;                   // Spawned=0 (advisory), Died=1
    u8  reserved;
};                                 // 8 B
```

- **Sender** (every machine, own room only): enumerates local enemy/boss
  actors (room-change scan immediate + 15-frame backstop — the M4.6
  cadence work carries over), broadcasts GhostSnapshot at:
  **enemies 15 Hz, bosses 30 Hz, projectiles 60 Hz** (per-type cadence,
  one `NetClock::AtRate` gate each). Death detection reuses the M4.6
  `PollHostDeaths`-equivalent, renamed `PollOwnDeaths` — the adapter-on-`
  entry UAF fix (store `s16 procName` + death-test callback at registration,
  never deref after `gone`) carries over, minus the switch/drop reads.
- **Receiver**: `GhostRegistry` keyed `(senderId, entityId)` → ghost actor
  handle. First sight → spawn; per packet → apply; `Died` event or **1 s
  silence TTL** → despawn (the TTL covers sender stage-switches and lost
  unreliable packets — no reliable presence protocol). Sender leaves /
  session ends → clear all their ghosts.

### 4.3 The generic ghost actor (new procName, e.g. `coop_ghost`)

One new actor class, spawned by `fopAcM_create` per remote entity:

- **Model**: loaded from the type's own arc/bdl — a small render-only table
  `{procName → arc, model name, anim name}` replaces `kAdapters`. Seed rows:
  the old whitelist (`E_AI, E_HM, E_DF, E_YC, E_MD`), bosses (`B_TN` +
  the boss profile list), and the common stage enemy types; unknown types
  are skipped + counter-logged (never crash). Models' arcs are stage-
  independent, so **cross-stage and cross-room ghosts render identically**.
- **Pose**: position/angle from the packet; animation applied by a per-type
  anim-apply callback (the slim survivor of the old `driveModel` — morf
  frame set + baseMtx like the frozen-puppet path). Bosses may need a
  handful of key joints — v1 ships positional + anim only and documents
  crude boss limbs as accepted (a joint-augment packet is an M6 seam).
- **Render**: translucent — per-material alpha override (~100/255) on the
  ghost's J3D model, standard TP translucent draw (PSTranslucent +
  material alpha). No shadow, no grabbable, no culling surprises within the
  room; cull ghosts beyond `kGhostCullDistance` (≈ 5000 units / stage- \
  area scale).
- **Interaction**: NO collision (no At/Tg/Co colliders), NO execute logic,
  NO damage intake, invisible to `fopAcM_searchPlayer*`-style queries —
  AI never knows ghosts exist. Ghosts never enter the local enemy registry,
  so ALLDIE/drops/story switches ignore them completely.
- **Cap**: per-sender ghost cap (unscientific but bounded, e.g. 24/sender),
  spawn-side reject above it.

### 4.4 Projectile ghosts

Player and enemy projectiles are ordinary fopAc actors — they ride the
**same GhostSnapshot channel** (type = projectile procName, `speed` filled,
`flags` = projectile bit, 60 Hz). Receiver-side the generic ghost actor
renders the projectile's model and advances by `speed` between packets
(lerp), with a short TTL (≈ 2 s) so a lost last packet can't leave a
stuck ghost. Seed rows: player **arrow, flaming arrow, boomerang, bomb
entity, clawshot**, and common enemy shots (E_AI rock, E_FB fire, etc.);
unknown projectile types → simple generic marker (small translucent sphere)
rather than a missing ghost.

### 4.5 Bandwidth

Enemies 15 Hz × 40 B, bosses 30 Hz × 40 B, projectiles 60 Hz × 40 B.
Worst room: ~30 enemies + 2 bosses + 15 projectiles ≈
(30×15 + 2×30 + 15×60) × 40 B ≈ **62 KB/s per sender**; 4-player shared
room ≈ 185 KB/s aggregate — versus the old ~1 MB/s worst case. PlayerState
(2017 B × 60 Hz) unchanged.

## 5. Milestones (M5 series, renumbered on the pivot)

- **M5.0 design review** — this doc through the adversarial pair; fixes folded
  in, then committed.
- **M5.1 SHRED** — delete/revert everything in §2; protocol v7;
  session/coop glue reduced to player+time/weather+roster; selftest
  rewritten (drop ownership/combat/room-scope suites, keep protocol/envelope/
  transport/session-lifecycle/player/time suites); forced rebuild green +
  selftest green with the ghost layer **stubbed** (v1 co-op = puppets +
  sky, zero ghosts). Risk-front-loaded: prove the simplified core stands
  alone before adding rendering.
- **M5.2 GHOST SENDER** — room entity enumeration, per-cadence
  GhostSnapshot broadcast, `PollOwnDeaths` → EnemyEvent(Died), sender cap,
  selftest (snapshot count/cadence/died rows, wire 40 B asserted).
- **M5.3 GHOST RECEIVER** — `coop_ghost` actor, render table, registry,
  spawn/apply/despawn/TTL, cross-stage + same-stage visibility, alpha/
  culling, selftest (registry lifecycle, TTL expiry, cap, unknown-type
  skip).
- **M5.4 PROJECTILE GHOSTS** — projectile rows, speed-advance lerp, TTL,
  generic-marker fallback.
- **M5.5 POLISH + CAPSTONE REVIEW** — transparency tuning, ghost-count and
  bandwidth telemetry, latency (15 Hz → visible stutter? lerp or 30 Hz
  boss-only bump), ghost/puppet/sky coexistence probing, full adversarial
  review, live two-instance playtest checklist (`tools/run-coop.sh`).

## 6. Risks & accepted trade-offs

1. **No shared combat** — your friend's copies cannot die to your sword;
   co-op is parallel-play + spectate. (User decision; assist is M6.)
2. **Double vision** in shared rooms — same-type copies overlap
   (yours solid, theirs ghosted). (User decision.)
3. **Crude boss ghosts** — positional + anim only, no limb mirroring in v1
   (documented; joint-augment packet is an M6 seam).
4. **Ghost model table maintenance** — new enemy types need a render row or
   they skip; the counter-log makes misses visible.
5. **All 60 Hz → 15 Hz behaviors** — ghost motion is choppier than the old
   pose sync; watchdog TTL covers packet loss. Bosses bumped to 30 Hz.
6. **Puppets remain invulnerable to ghosts** — consistent with "no damage
   to/from ghosts" (users may find it odd that a friend's boomerang passes
   through them; acceptable).

## 7. What the live playtest now probes (replaces the old list)

Same-room double vision + doors-per-player; cross-room/cross-stage ghost
visibility; ghost spawn/despawn on the sender's room change and kills; boss
ghost presence; player projectile ghosts (arrow/boomerang/bomb across
machines); enemy projectile ghosts; ghost cull distance; host-leave clears
ghosts; puppets unhittable by ghosts; time/weather still in lockstep;
rejoin-in-same-process still works (M4.6 MAJOR 1 fix regression); save
mtimes untouched.

## 8. M6 seams (documented, not built)

Assist combat (fold one machine's hits into another's sim via a slim
CombatIntent — the old M2 machinery is the reference), dynamic
waves/spawns, horse ghosts + mount sync, boss limb-augment packets,
ghost HP bars.