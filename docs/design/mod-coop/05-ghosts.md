# 05 — Ghosts: parallel-world co-op (the M5 pivot)

Status: **DESIGN v2 (M5.0 approved pending) — BLOCKER/MAJOR findings from
the adversarial design reviews folded in.**
Review records: `review-m5-design-deepseek-v4-flash-0731.md`,
`review-m5-design-glm-5.2.md` (both committed — every B/F/M number below
maps to them; "d#" = deepseek, "g#" = glm).
Supersedes for M5+: the owner-authoritative enemy/combat direction of
`implementation-plan.md` M2/M3/M4, `m2-design-notes.md`, `03-enemies.md`,
and `00-network.md` §6 (room ownership). The transport/session/protocol
core (00-network.md §1–§5) and player/time/weather subsystems are
unchanged.

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

v1 is *co-op play + spectate*, not *shared combat*. A minimal assist path
(fold your hits into your friend's sim) is a documented M6 seam, not built
now. Both reviewers confirmed the pivot itself is the right call; the
authority stack was the highest memory-unsafety surface in the tree (five
classes of pointer/lifetime hazards defended by comments, not invariants).

## 2. What dies (concrete, per file — M5.1 shred)

**Deleted or reverted to vanilla in M5.1.** The acceptance of M5.1 is a
**grep-driven checklist over this table**, not just "build green".

| Artifact | Where | Disposition |
|---|---|---|
| `RoomOwnershipMsg` + `RoomOwnershipTable` + `OwnerOf` + `UpdatePlayerRoom`/`OnPlayerEnter(Leave)` + transfer/join-map + `playerRoom_[]` + `ownership_` | protocol.h, session.cpp | **delete** |
| Host room sniff in the shared PlayerState/PlayerEvent receive path (`UpdatePlayerRoom` call sites) | session.cpp | **delete** |
| `SendToAllInRoom`, `RouteCombatIntent`, `RelayPolicy::RoomScoped` branch of `ForwardGameMessage`/`SendGameMessage`, combat `PolicyFor` cases | session.cpp | **delete** (ghosts broadcast `SendToAll`; the receive gate §4.3 is the only room filter) |
| CombatIntentMsg, CombatResultMsg + routing/validation, `SetTgHitSynthetic` Tg-flag/synth collider injection, `applyInjectedHit` | protocol.h, coop_combat.cpp, coop_enemy.cpp, **d_cc_s.cpp** (the ONLY combat intercept, :540 `noteAtTgHit` — see gF8) | **delete-file / revert-hook** |
| Enemy freeze: `puppetExecute` pre-guard + `ScopedEnemyTarget` push (:357) + `hostOnExecuted` post-execute (:377) blocks in f_op_actor.cpp | f_op_actor.cpp | **delete** (nothing is ever frozen — every enemy is local) |
| Full `NetEnemyAdapter` combat surface: `injectHit`, `deathSwitchNo`, `dropTableId`, `defaultPowerType`, `refreshColliders`, `DamageSemantics` | coop_enemy.cpp `kAdapters` | **delete** — replaced by a slim sender-only table (gF6, §4.2) + the receiver render table (§4.3) |
| Drops gates/item distribution (`SpawnDropAndBeat`/`GrantLocalSwitch`) — note: **`coop_drops.cpp` does NOT exist**; the drop code lives in coop_enemy.cpp (dM3) | coop_enemy.cpp | **delete** (drops fully local per player's own kills) |
| Room-clear sync: `clientRoomCleared`/`hostRoomCleared` + d_a_alldie.cpp hook + `g_roomClear`/`g_roomHadEnemies` | d_a_alldie.cpp, coop_enemy.cpp | **delete** (ALLDIE local; doors open per player) |
| D3 targeting: `ScopedEnemyTarget`/`currentTargetPlayer`/`resolveNearestPlayer`, f_op_actor_mng.h `fopAcM_getContextPlayer` shim + 10 searchActor wrappers (removable, only E_YC/E_AI call them), d_a_player.h `daPy_get*ActorClass` shims, the 6 E_YC + 3 E_AI read-routing sites | coop_context.cpp (whole file), f_op_actor_mng.h, d_a_player.h, d_a_e_yc.cpp, d_a_e_ai.cpp | **delete / revert to vanilla** (enemies always target their local slot-0 Link) |
| Dead coop.h exports after the shred: `amIRoomOwner`, `remoteInRoom`, `puppetActorFor`, `resolveNearestPlayer` (dM1) | coop.h/coop.cpp | **delete** |
| Stale protocol.h v7 comment ("adds spawn params, horse channel") + M4.5 capstone "M5 decides wire-vs-drop" notes | protocol.h | **rewrite** (the real v7 is this doc's breaking deletion; the wire-vs-drop notes resolve as *dropped*) |
| `EnemyEventMsg` excess: `data`/`flags`/`flagMask` fields, `Spawned`/`RoomClear`/`BossPhase` events | protocol.h | **trim** (gF12/F13, m2): Died only — see §4.2 |

**NOT in scope — keep intact:** transport/session/discovery/config/shutdown,
PlayerState puppets + all `isPuppet` guards (create-path save guards,
invulnerability, hidden draw, weapon bones), forms, horses, time/weather,
per-player save system, protocol envelope/serializer core.
**d_cc_uty.cpp is NOT shredded** (gF8): its `TARGET_PC` blocks are the
`invincibleEnemies` settings cheat + achievement signals — fork features,
not coop combat intercepts. **d_attention.cpp stays** (its :655 lock-on
guard is M1 puppet code that survives).

**Session survival boundary (gF15):** session retains `roster_`,
`worldStage_`, the per-receiver `g_receive[].state` room tracking in
coop.cpp, PlayerLeave handling, and the M4.6 transport-teardown lifecycle.
Session loses: `playerRoom_[]`, `ownership_`/`RoomOwnershipTable`,
`UpdatePlayerRoom`, `SendToAllInRoom`, `RouteCombatIntent`, the
`RelayPolicy::RoomScoped` branch, combat `PolicyFor` cases. The surviving
PlayerState sender gate reads `g_receive[].state` directly (dormant-kept;
it is NOT ownership machinery and survives).

## 3. Wire changes (kProtocolVersion → **7**)

v7 is a **breaking deletion** (combat/ownership types out) plus the ghost
types in (§4.2). Type count 16 → **13** (`CombatIntent`, `CombatResult`,
`RoomOwnership` removed; `EnemySnapshot` → `GhostSnapshot`; `EnemyEvent`
trimmed). `PayloadUnion`, `WireSize`, `ChannelFor`, and the determinism
sweep (now over 13 types) are updated to match. `kMaxMessageSize = 4096`
unchanged. The version-history comment replaces the stale v7 promise
(dM1) with this one.

## 4. Ghost layer

### 4.1 Identity — `GhostEntityId` (sender-major in the PACKET)

`StageEntityId` = `(room<<8)|setID` stays for stage-placed enemies;
dynamic spawns use owner-major `(owner<<12)|counter` — the existing
`coop_entity_logic.h` scheme, **kept and repurposed** (gF14/dM2):
`registerDynamicEnemy` goes LIVE in M5.2 so the sender can id dynamic
spawns (script-spawned Bokoblins are ghost sources on day one), and
M5.4's projectiles ride the same dynamic path (see §4.4 step 0).

**The wire carries `senderId` explicitly** (d-B1/gF5): on the host, origin
is known, but clients only ever receive from peer 0 (star topology) and
the relay forwards payloads verbatim — no origin stamping exists today.
The receiver keys its registry on **`(senderId, entityId)`**, so a
same-room pair's identical `(room<<8)|setID` never collides, and the host
drops `senderId == selfId()` packets (self-echo guard — the host consumes
its own messages then relays).

### 4.2 Messages (fixed-size, hand-serialized — same contract as v6)

```cpp
struct GhostSnapshotMsg {          // unreliable (channel 1)
    u8  senderId;                  // session PlayerId — receiver registry key
    u8  flags;                     // bit0 boss; bit1 projectile; v1: rest 0
    u16 entityId;                  // (room<<8)|setID, or owner-major dynamic id
    u16 type;                      // procName (profile id)
    s16 angle;                     // shape_angle.y
    u16 animFrame;                 // v1: morf frame only (see B2 — frame-only fidelity)
    Vec3f pos;
    Vec3f speed;                   // knockback / projectiles
};                                 // 1+1+2+2+2+2+12+12 = 34 B payload, +4 B envelope = 38 B

struct EnemyEventMsg {             // reliable (channel 0), trimmed to Died only
    u8  senderId;
    u8  eventId;                   // EnemyEventId::Died
    u16 entityId;
};                                 // 4 B payload
```

- `anim`: the sender packs **frame only** (`morf->getFrame()`). The high 16
  bits that v4 promised for action ids have been wire-zero since v4
  (`PackAnim` never transmitted an action id — d-B2). Action-id packing is
  deferred to the M6 joint-augment seam. Consequence, stated plainly:
  **every ghost of a type renders its ONE bound bck (see §4.3), frame-
  synced**; E_YC's fly/hover/attack are indistinguishable, and frame
  clamps to the bck's length cause stutter. Accepted for v1 (gF11: no anim
  lerp; position lerp only).
- No `Spawned` event, no `flags` dead-gone bit (gF12/F13): spawns are
  implicit-by-first-snapshot, despawns are Died-or-TTL.

### 4.3 Receiver — `coop_ghost` actor (new procName)

- **Profile contract (d-M3/gF4):** `Group = fopAc_ACTOR_e` — **NEVER
  `fopAc_ENEMY_e`** — and no status bits that draw enemy scans. ALLDIE,
  `d_a_door_shutter/spiral`, `d_a_tbox`, and the candle-demo tag scan
  `fopAcM_myRoomSearchEnemy` by **group, not the coop registry**; a ghost
  in the enemy group would block doors/chests forever. **d_attention
  exclusion:** `coop_ghost` actors are never added to any attention/
  lock-on target list — same shape as the existing puppet exclusion
  (d_attention.cpp:655), gated on `isGhost(actor)`.
- **Receive gate (d-M1/gF1):** spawn ONLY when the sender is confirmed
  **same stage AND same room**, cross-referencing the puppet layer's
  stream: `g_receive[sender].hasState && strcmp(g_receive[sender].state.
  stage, localStage)==0 && g_receive[sender].state.roomNo == localRoomNo`.
  Snapshots from a sender with no state, or in a different stage/room, are
  counted + dropped — **never spawned**. Cross-stage ghosts are geo-
  garbage by definition (room numbers aren't stage-unique); cross-room
  (same stage, adjacent rooms) is the only cross-room case and IS rendered.
  This gate also guarantees the ghost's arc is resident on the receiver
  (same stage ⇒ same enemy population ⇒ same arcs already mounted) and
  that a sender hidden as a puppet (different stage) spawns no ghosts —
  **ghost×puppet coexistence stays consistent because both key on the
  same `g_receive[sender].state`** (gF17).
- **Local-copy suppression (user refinement 2025):** a ghost for a
  STAGE-PLACED entity is hidden while the receiver's OWN copy of that
  entity is alive. Suppression set = the receiver's own sender-side
  enumeration (`StageEntityId` → alive local actor), which every machine
  already builds to broadcast its own room — so the check is a
  **map-membership test, zero extra wire**. The ghost actor stays alive
  in the registry but its **draw is skipped** while suppressed (no
  spawn/despawn thrash when both copies die within a frame of each
  other); on the local copy's death (actor gone or `isDead` per the
  sender-side table) the setID leaves the map → the ghost appears so you
  watch the remote's surviving copy. Dynamic spawns and projectiles
  (owner-major dynamic ids, no setID overlap) are **never suppressed** —
  their spatial overlap at the same scripted point is the watchable
  thing. Accepted asymmetry: while both copies live you see only YOUR
  fight in a shared room (double-vision now occurs only after your copy
  dies, or across rooms).
- **Spawn path:** `fopAcM_create` per ghost (bought: free drawlist + cull
  integration; costs: heap + cPhs phases per actor). **Spawn storm
  pacing**: ≤ 8 ghost creates/frame (a 24-ghost first sight spreads over
  3 frames). Per-sender cap **24 ghosts + 12 projectiles** (separate
  budgets; count projectiles against their own cap, d-M4).
- **Render table (d-B2/gF3 — honest model-init):** this is per-type model
  CONSTRUCTION, not a slim callback. Row shape:
  `{procName, arcName, modelDataResId, bckResId, morfKind
  (McaMorfSO|J3DModel+J3DAnmChr), buildFn, frameFn, isDead}` — `buildFn`
  replicates what each enemy's `create()` does for its model (resLoad the
  arc, `dComIfG_getObjectRes`, construct the morf from the right
  bck/vaf, wire baseTRMtx) minus colliders/AI; `frameFn` sets the morf
  frame + baseMtx. Alternative (heavier, rejected for v1): one ghost class
  per type subclassing the real enemy with execute/colliders neutered.
  **Multi-model rows exist:** `B_TN` has TWO arcs
  (`dComIfG_resLoad("B_tn")` + `mArcName`); E_YC's wolf-bite/Midna rider
  is a separate `E_RDY` actor (d-B2). Seed rows: `E_AI, E_HM, E_DF,
  E_YC, E_MD`, bosses (`B_TN, B_MT, B_MZ, B_ML, B_DG, B_GZ, B_ST` + the
  boss profile list per dStage), the enemy profile list for common stage
  types (Bokoblin class `E_BB`, Keese `E_KF`, fire/ice `E_FB`+E_IZ,
  Skulltula etc.), and projectiles (§4.4). Unknown types: skip + counter
  -log (never crash). **M5.3 needs the actual seeded table with real
  resIds** — a day-one task, not a polish item (gF17).
- **Rendering (d-B3/gF2 — the alpha fix):** TP's `J3DModelData` is SHARED
  per arc (`dComIfG_getObjectRes` cache) — mutating material alpha on the
  ghost's modelData would turn the receiver's OWN real same-type enemies
  translucent. **Never mutate shared modelData.** Use per-instance cloned
  materials: `J3DMaterial::copy()` per mesh + `J3DMatPacket::setMaterial()`
  (clone the type's material set ONCE, stamp alpha once, share the cloned
  set among all ghosts of that type), alpha ≈ 100/255, PSTranslucent draw.
  Memory bound: `modelDataSize × 1 clone × ghost count` is small (one
  clone per TYPE, not per ghost). M5.3b acceptance includes: a ghost and a
  real same-type enemy co-render with the real one **opaque**.
- **Scale (d-m3):** ghosts adopt the receiver's local same-type actor's
  scale (same stage data ⇒ same setID/HIO ⇒ correct by construction);
  HIO-divergence caveat documented. No scale field on the wire.
- **Despawn:** Died event, or **1 s silence TTL** (covers lost packets,
  sender menu-open/load pauses — those pop ghosts for up to ~1 s + load
  time, then respawn on the re-announce; accepted, d-m6). TTL expiry, not
  Died, covers the lost-PlayerLeave case on host crash (gF10).
- **Cleanup:** `GhostRegistry::clearSender(senderId)` called from the
  `PlayerLeave` handler and on session end (gF10).
- **Cull:** ghosts beyond `kGhostCullDistance` (≈ 5000) skip draw (stage-
  area scale).

### 4.4 Projectile ghosts

Ride the **same GhostSnapshot channel** (`flags` projectile bit, `speed`
set, 60 Hz). **M5.4 step 0 (d-M2/gF14):** projectiles are dynamic spawns
(`setID == 0xFFFF`) that outlive the 15-frame scan — the sender needs a
**per-frame transient scan** of projectile profiles (not the room-change
scan) + live `registerDynamicEnemy` ids. Receiver: generic ghost actor,
model from the projectile rows, **position advanced by `speed` between
packets (lerp)** — the ONLY lerped ghosts (gF9); short TTL ≈ 2 s; unknown
projectile types render a generic marker (small translucent sphere), never
nothing. Seed rows: player arrow/flaming-arrow/boomerang/bomb/clawshot,
common enemy shots (E_AI rock, E_FB fire, E_GK/Deku?). Projectiles ghost
**through walls** (speed-lerp has no collision) — accepted; stated in §7
so the playtest records it as expected.

**Cadence (gF9/d-m4):** per-row, not flat — walkers 15 Hz, **fast
lungers/flyers 30 Hz** (E_AI knights, E_YC, E_HM, E_DF, bosses), projectiles
60 Hz. Optional receiver-side position lerp for ALL ghosts as M5.5 polish.

### 4.5 Bandwidth (honest — gF5/F7, d-M5)

Payload 34 B + 4 B envelope = **38 B**. Per-frame per-sender ≈ (30×15/60
+ 2×30/60 + 15×60/60) ≈ 23.5 pkts → ≈ **0.9 KB/frame ≈ 54 KB/s per
sender**. **The ghost layer is O(player count)** — every machine
broadcasts its own room:

| Case | Ghosts | PlayerState | Aggregate |
|---|---|---|---|
| 2 players, shared room | ~107 KB/s | ~241 KB/s | ~0.35 MB/s |
| 4 players, shared room | ~214 KB/s | ~483 KB/s | ~0.7 MB/s |
| 8 players, worst room | ~429 KB/s | ~966 KB/s | **~1.4 MB/s** |

Honest framing: PlayerState pose traffic is UNCHANGED (it was always the
~1 MB/s article); the ghost layer starts at ~106 KB/s for 2 players and
scales linearly with N — at 8 players it rivals the old owned-enemy
stream's worst case. All LAN-trivial, but the **inbound snapshot ring
(`kSnapshotRingCapacity = 128`, ONE SPSC fed by all peers) overflows
systematically at ≥5 players in a shared room with projectiles** (8
senders × ~24 ghost msgs/frame > 128) → raise the capacity AND cap
projectile cadence at 30 Hz for >4 players (gF7). M5.5 adds a snapshot-ring
drop-counter telemetry row. (Enemy-only bursts themselves leave >100
frames of headroom; ring overflow is a ≥5-player projectile issue.)

## 5. Milestones (M5 series)

- **M5.0 design review — DONE after this revision.** Acceptance: this doc +
  the two review files committed; §2 shred table corrected per the
  attachment trace; BLOCKER/MAJOR fixes folded (§3/§4). Doc-only; M5.0's
  finished proof is the M5.1 build.
- **M5.1 SHRED** — everything in §2, protocol v7, `PayloadUnion`/
  `WireSize`/`ChannelFor`/determinism over 13 types, dead exports removed,
  stale comments rewritten. **Acceptance (grep-driven checklist over the
  §2 table, not just "build green"):** every row verified absent; forced
  rebuild green (touched TUs warning-free); selftest rewritten: **drop**
  `RunM2RelayPolicyCheck`, `RunM4OwnershipTableCheck`,
  `RunM4RoomRoutingCheck`, `RunM4EntityStabilityCheck`, the M4 ownership
  parts of `RunM4WorldStageCheck`; **keep** `RunProtocolChecks`,
  `RunDeterminismChecks` (extended to the 13-type set), `RunHandshakeDemo`,
  `RunM3TimeWeatherCheck`, `RunM46SessionRestartCheck`,
  `RunM4DiscoveryCheck`; **add** a v7 `WireSize`/round-trip row for
  GhostSnapshot (34 B) + trimmed EnemyEvent (4 B); **net-off regression
  row** (d-m8): `net.enabled=false` → zero ghost sends, zero registry
  entries, `myRoomSearchEnemy` bit-identical to vanilla boot. Ghost layer
  **stubbed** (v1 co-op = puppets + sky).
- **M5.2 GHOST SENDER** — room entity enumeration (immediate room-change
  scan + 15-frame backstop, existing cadence), live `registerDynamicEnemy`
  for dynamic spawns, per-row cadence send (15/30 Hz), slim sender-only
  `{procName, isDead}` death table (gF6: NOT the full adapter — no
  combat/switch/drop reads; the M4.6 UAF discipline becomes **store
  `procName` + slim `isDead` at registration, never deref the actor after
  `gone`**), sender hook runs **un-gated on every machine** (reads
  pre-execute in `onGameFrame`'s scan — 1-frame-stale pose, accepted,
  d-M6), EnemyEvent(Died) broadcast, senderId stamping + self-echo drop on
  the host. Selftest: snapshot count/cadence, 34-B wire asserted, Died
  rows, self-echo drop.
- **M5.3 RECEIVER — split into two (d §5):**
  - **M5.3a** registry + spawn/apply/despawn/TTL/cap + receive gate (§4.3)
    with a placeholder box model: proves lifecycle, the ALLDIE-group check
    (a ghost in a room does NOT block ACT_TIMER), the attention exclusion,
    the stage/room gate, and **local-copy suppression** (local alive →
    ghost draw skipped; local dies → ghost appears; no thrash; dynamic/
    projectile ids never suppressed). Selftest-driven.
  - **M5.3b** the render table + morf construction + cloned-material alpha:
    proves the std-code claims (B2/B3). Per-type visual acceptance
    (ghost + real enemy co-render, real stays opaque; B_TN two arcs; E_YC
    rider documented-crude).
- **M5.4 PROJECTILE GHOSTS** — step 0 (per-frame transient scan + dynamic
  ids), rows, speed-lerp, 60 Hz cadence + >4-player cap, ring-overflow
  guard (assert snapshot-ring drop counter stays 0 under burst), generic-
  marker fallback.
- **M5.5 POLISH + CAPSTONE REVIEW** — transparency tuning, ghost-count/
  ring-drop/bandwidth telemetry, optional all-ghost position lerp,
  ghost/puppet/sky coexistence probing, full adversarial review, live
  two-instance playtest checklist (`tools/run-coop.sh`).

## 6. Risks & accepted trade-offs (d §4, gF9/F10)

1. **No shared combat** — your friend's copies cannot die to your sword;
   co-op is parallel-play + spectate. (User decision; assist is M6.)
2. **Double vision, suppressed while your copy lives (user refinement
   2025)** — in a shared room the remote ghost of a stage-placed entity
   renders only when YOUR copy is dead or missing (§4.3 suppression);
   while both live you see only your own fight. Cross-room ghosts always
   render. Residual overlap: same-type dynamic spawns at the same
   scripted point (never suppressed, v1).
3. **Anim-fidelity collapse** — every ghost of a type plays its ONE bck,
   frame-synced; fast-enemy ghosts can stutter (frame clamp; 15 Hz with no
   lerp). This is the single most visible artifact of the pivot. Mitigant:
   per-row 30 Hz for fast types; optional lerp at M5.5. (d-B2, gF9/F11)
4. **Ghost crowd vs room actor budget** — 4 players ≈ ~100 actors in one
   room (30 real + 3×24 ghosts); 8 players ≈ ~192 ghosts. Rooms were built
   for 10–30. Spawn pacing (≤8/frame) + caps contain the create storm;
   draw cost is the load hazard the telemetry guards. (d §4, gF16)
5. **Crude boss ghosts** — positional + frame-only anim, no limb mirroring;
   B_TN's second arc / E_YC's invisible rider are documented degradations.
   (joint-augment packet is the M6 seam.)
6. **Ring overflow at ≥5 players** in a shared room with projectiles
   (gF7) — capacity raise + projectile cadence cap.
7. **Sender pauses/loads pop ghosts** for ~1 s + load time (TTL), then
   respawn. (d-m6)
8. **Ghosts have no Co collider** — you walk through a ghosted Darknut
   with zero feedback; ghosts pass through walls. Consistent, but a
   first-play surprise; state it in the playtest.
9. **ALLDIE-door hazard** — REQUIRES the §4.3 group contract
   (`fopAc_ACTOR_e`); a wrong group blocks doors/chests/candle-demos
   forever. Selftested in M5.3a. (gF4)
10. **Per-type table maintenance** — new types need a render row + death
    test or they skip; the counter-log makes misses visible.

## 7. What the live playtest now probes (replaces the old list)

Same-room **suppression** + doors-per-player; **cross-room (same stage)
ghost visibility** — NOT cross-stage (gate drops those, gF1/d-M1);
**suppression dynamics**: with both copies alive the ghost is hidden, kill
YOUR copy → the remote's ghost appears (their copy survives), kill THEIR
copy first → Died despawns the ghost; late-join room entry (ghost visible
before your copy spawns) then your copy spawns → suppression engages;
ghost spawn/despawn on the sender's room change and kills (including the
~1 s pop on sender load); boss ghost presence (frame-only, crude limbs —
expected); **fast-enemy ghost smoothness** in a shared room (kargorok/
knight — accept or bump cadence, gF9); player projectile ghosts
(arrow/boomerang/bomb across machines, including **ghost-through-walls** —
expected); enemy projectile ghosts; ghost cull distance; a ghost in an
ALLDIE room does NOT block the door; ghosts never appear in the lock-on
list; ghosts pop/return when the sender pauses or loads; host-leave clears
all ghosts (and rejoin-in-same-process — M4.6 regression); puppets
unhittable by ghosts; time/weather still in lockstep; save mtimes
untouched; `net.enabled=false` boots byte-for-byte vanilla.

## 8. M6 seams (documented, not built)

Assist combat (fold one machine's hits into another's sim via a slim
CombatIntent — the old M2 machinery is the reference), dynamic waves
(Spawned event returns), horse ghosts + mount sync, boss limb-augment
packets + anim action-id packing, ghost HP bars, cross-stage spectate
rendering (meaningful positions require a sender-world remap — deferred
on purpose).