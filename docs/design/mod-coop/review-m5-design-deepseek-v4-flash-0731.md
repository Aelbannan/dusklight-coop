# Adversarial design review — 05-ghosts.md (the M5 pivot)

**Subject:** `docs/design/mod-coop/05-ghosts.md` — parallel-worlds co-op with ghost mirrors.
**Reviewer:** deepseek-v4-flash-0731 (adversarial pass, branch `net-coop` @ 7691015066).
**Scope:** shred completeness vs the real `#if TARGET_PC` attachment points, v7 wire plan, ghost
receiver feasibility against the std code (J3D model/morf/alpha surface), cadence math, risk
framing, milestone shape, doc gaps. Read-only review — no source modified.

---

## 1. VERDICT

**Is the pivot sound? YES.** The M2/M4 authority stack (freeze/apply, synthetic-hit injection,
room ownership, room-scoped relay) was the highest-risk surface in the tree — five classes of
memory-unsafety (`SetTgHitSynthetic` pointer lifetimes, `PollHostDeaths` UAF, cullMtx per-entry,
synthAt per-entry) were defended by comments, not invariants. Deleting it for per-machine vanilla
simulation is the correct call for LAN co-op, it matches Anchor's proven "no enemy sync"
precedent, and the "keep puppets + time/weather" core is genuinely untouched by the shred (I
verified every `d_a_alink.cpp` guard is `isPuppet`-driven M1 code that survives; `d_attention.cpp`
hooks are M1/generic and survive; `f_op_actor_mng.cpp` and `d_cc_uty.cpp` have no coop hooks at
all).

**Is the shred complete? MOSTLY — with real holes.** The *intent* is complete (every authority
artifact is named), but the *row-level mapping* has errors (a file that doesn't exist, a file
listed that has nothing to shred), it omits dangling coop.h exports and the f_op_actor.cpp
context/hostOnExecuted blocks from its "Where" column, and it leaves a stale v7 version-history
comment in protocol.h describing a *different* v7. A developer executing the table mechanically
will either grep and find nothing (d_cc_uty.cpp, coop_drops.cpp) or leave dead surface
(`amIRoomOwner`, `remoteInRoom`, `resolveNearestPlayer`, `puppetActorFor`). See §2 and B/M
findings.

**Is the ghost receiver buildable against the real code? YES with two blocking fixes and one
honesty correction.** The std tree has everything the doc needs: `mDoExt_McaMorfSO` has a public
constructor (`include/m_Do/m_Do_ext.h:411`), per-instance material override is possible via
`J3DMaterial::copy` + `J3DMatPacket::setMaterial` (`J3DMaterial.h:33`, `J3DPacket.h:299`),
`NetClock::AtRate` exists for per-cadence gates, the entity-id scheme exists in
`coop_entity_logic.h`, and `dComIfG_resLoad`/`dComIfG_getObjectRes` give the arc/bck lookup the
render table needs. But: (1) **GhostSnapshotMsg has no sender-id field** yet the receiver keys by
`(sender PlayerId, entityId)` — clients cannot know a relayed message's origin today (B1);
(2) **the alpha override as written wrecks shared J3DModelData** — same-room real enemies share
the ghost's model data (B3); (3) the **anim contract overstates feasibility** — the sender has
never transmitted an action id (`PackAnim` returns only the morf frame; the high 16 bits of
`anim` are wire-zero today), so the doc's "action id u16 | frame u16" and "anim-apply like the
frozen-puppet path" do not survive contact with the code (B2). Everything else is buildable with
documented effort.

---

## 2. SHRED TRACE — every attachment point, disposition check

Verified against the tree (grep `#if TARGET_PC` + reading each site). ✓ = doc is right,
✗ = doc wrong/gap.

| File | Coop hooks today | Doc says | Check |
|---|---|---|---|
| `src/f_op/f_op_actor.cpp` | :296 `puppetExecute` pre-guard; :357 `ScopedEnemyTarget` context push; :377 `hostOnExecuted` post-execute | :296 listed (freeze row); **:357/:377 NOT in the table's Where column** (they live in the D3 row's *spirit*, not its file list) | ✗ gap |
| `src/d/actor/d_a_alldie.cpp` | `hostRoomCleared` (actionCheck), `clientRoomClearGated` (actionCheck + actionTimer) | listed, delete | ✓ |
| `src/d/d_cc_s.cpp` | :540 `noteAtTgHit` in `SetAtTgGObjInf` — the ONLY combat intercept | listed with `d_cc_uty.cpp` | ✗ half-wrong: d_cc_uty has NO coop hook (its TARGET_PC blocks are achievements/settings, they survive) |
| `src/d/d_cc_uty.cpp` | none (achievements + `invincibleEnemies` setting only) | "delete/revert" | ✗ nothing to shred; revert not needed |
| `src/d/actor/d_a_e_yc.cpp` | 6 D3 sites (`damage_check`, `e_yc_f_fly`, `e_yc_hovering`, `e_yc_attack`, `e_yc_wolfbite`, `daE_YC_Execute`) | listed, revert | ✓ |
| `src/d/actor/d_a_e_ai.cpp` | 3 D3 sites (`player_way_check`, `pl_check`, `damage_check`) | listed, revert | ✓ |
| `include/f_op/f_op_actor_mng.h` | `fopAcM_getContextPlayer` shim (:728-735) + 10 searchActor wrappers — **only E_YC/E_AI call it** | listed | ✓ (shim becomes fully removable, not just reverted) |
| `include/d/actor/d_a_player.h` | `daPy_getPlayerActorClass`/`daPy_getLinkPlayerActorClass` D3 shims (:1289) | listed | ✓ |
| `src/dusk/coop/coop_enemy.cpp` | registry, kAdapters, freeze/apply, inject, drops (`SpawnDropAndBeat`/`GrantLocalSwitch`), room-clear, `registerDynamicEnemy` | rows exist, but **"coop_drops.cpp" does not exist** — the drop code lives here | ✗ file name error |
| `src/dusk/coop/coop_combat.cpp` | intent capture/validation/flush | listed | ✓ (whole file dies) |
| `src/dusk/coop/coop_context.cpp` | `ScopedEnemyTarget`, `currentTargetPlayer`, `resolveNearestPlayer` | listed | ✓ (whole file dies) |
| `src/dusk/net/session.cpp` | `RoomOwnershipTable` (whole class), `playerRoom_`, `ownership_`, `UpdatePlayerRoom`, host sniff in the PlayerState/PlayerEvent receive path, `RouteCombatIntent`, `BroadcastOwnership`/`SendOwnershipMap`/`OnRoomOwnership`, `SendToAllInRoom`, `PolicyFor`/`RelayPolicy::RoomScoped` | table lists the class + SendToAllInRoom, but not the sniff in the shared receive path nor the relay-policy seam (`ForwardGameMessage`/`SendGameMessage` room-scope branches) | ✗ gap (rows named, but the receive-path sniff and the two relay functions are easy to miss) |
| `include/dusk/coop/coop.h` + `coop.cpp` | `amIRoomOwner`, `remoteInRoom`, `puppetActorFor`, `resolveNearestPlayer` (via context) | **not listed** — all become dead after the shred (their only consumers are the deleted enemy/combat modules) | ✗ gap |
| `include/dusk/net/protocol.h` | v6 structs | version-history comment at the bottom says *"M5 bumps to v7 (it adds wire fields: spawn params, horse channel)"* — a **different v7** than the ghost doc's breaking-deletion v7 | ✗ stale comment must be rewritten in M5.1 |
| `src/dusk/net/selftest_main.cpp` | 361 checks incl. M4 suites (ownership transfer, same-room scoping, room-scoped relay, `WireSize(RoomOwnership)==20` asserts :408, determinism over 16 types) | "selftest rewritten (drop ownership/combat/room-scope suites)" | ✓ intent; missing: the v7 wire-size asserts + type-count change (16 → 13) must be itemized |
| `src/f_op/f_op_actor_mng.cpp`, `src/d/d_attention.cpp`, `src/d/actor/d_a_alink.cpp` | no coop enemy hooks (puppet guards only) | "survives intact" | ✓ (attention lock-on guard at d_attention.cpp:655 is M1 puppet code — survives) |

**Shred-completeness verdict:** the *surface* is right, but three things are left dangling that
the doc's "the tree builds with PUPPETS + TIME/WEATHER ONLY" acceptance will silently tolerate
instead of explicitly removing: dead coop.h exports, the two f_op_actor.cpp blocks not named in
the table, and the protocol.h stale-v7 comment. The M5.1 acceptance should be a **grep-driven
checklist** (each row of this table verified), not just "build green".

---

## 3. RANKED FINDINGS

### BLOCKER B1 — GhostSnapshotMsg has no sender id; the receiver cannot key ghosts on a client
**Doc section:** §4.1 (identity `(sender PlayerId, entityId)`) vs §4.2 (the struct).
**Problem:** the struct is `{u16 entityId, u16 type, u8 flags, s16 angle, u32 anim, Vec3f pos,
Vec3f speed}` — no `senderId`. On the host, origin is known (`peerToPlayer_`), but **clients only
ever receive from peer 0** (star topology) and `Session::HandleData` forwards the payload
verbatim to the game handler (`session.cpp` `gameHandler_(msg.type, msg.payload)`) — there is no
origin stamping today (`PlayerStateMsg` carries `playerId` for exactly this reason; the old
`EnemySnapshot` never needed one because it was single-owner-scoped). A receiver with two remote
players in its room receives two GhostSnapshots with the same `(room<<8)|setID` and *cannot tell
them apart* — §4.1's whole "double-vision by construction, no registry collision" argument
collapses on clients. Related: the wire math in §4.2 is wrong — the listed fields pack to **35 B**
(2+2+1+2+4+12+12) via the byte-sequential `ByteWriter`, and the doc's own "5+4+1+2+4+12+12=40"
doesn't match its field list (natural C alignment would pad to 40, but the hand serializers never
align — `WireSize` and the determinism sweep use the packed number).
**Fix:** add `u8 senderId` (session PlayerId) to `GhostSnapshotMsg` (36 B packed; 41 B aligned —
state which in `WireSize`); the sender stamps `selfId()`, the receiver keys `(senderId, entityId)`
and ALSO gates on `senderId != selfId()` (a loopback relay echo must not ghost ourselves — the
host consumes its own messages via `gameHandler_` then relays, so the host must drop
`senderId == selfId()` packets; today the enemy module had no such guard because it never
received its own).

### BLOCKER B2 — The anim contract is unwireable as specified; ghost anims will all be one bck
**Doc section:** §4.2 (`anim` = "per-type packed: action id u16 | frame u16") and §4.3 ("animation
applied by a per-type anim-apply callback ... morf frame set + baseMtx like the frozen-puppet
path").
**Problem:** the sender has **never transmitted an action id**. `PackAnim()` (`coop_enemy.cpp`)
returns only `morf->getFrame()` as a u16 into the u32 `anim` — the high 16 bits are wire-zero
today, and no per-type action-id reader exists (action enums like `e_ai_class::ANM_*` /
`e_yc_class::ANM_*` are private per-class members; the kAdapters being deleted never captured
them). The frozen-puppet path worked because the *actor instance existed* with its own native
animation state machine already playing — the apply only overrode the morf frame. A bare
`coop_ghost` actor has **no playing animation**: the render table's single `anim name` bck is the
only animation it will ever show, at a synced frame. Two consequences the doc does not state:
(a) every ghost of a type renders one bck (a room of Armos ghosts all doing the same walk at
random frames; E_YC's fly/hover/attack/wolfbite are indistinguishable); (b) bck frame counts
differ per action, so `setFrame(syncedFrame)` clamps into the wrong length and ghosts stutter.
Also "the slim survivor of the old driveModel" is false: the old driveModel called the actor's
PUBLIC `setBaseMtx()`/`mtx_set()` (which know the actor's own model/scale) — the ghost has none
of that, and `mDoExt_McaMorfSO` construction (`m_Do_ext.h:411`) needs a `J3DAnmTransform*` bck +
callbacks per type.
**Fix (two honest options):** v1 ships **"one bound bck per type, frame-only fidelity"** — change
§4.2's comment to "anim = u16 frame only (v1)" and §4.3 to "row carries the one bck + per-type
morf construction (J3DModelData + J3DAnmTransform + nullptr callbacks), frame set only; action-id
packing deferred to the joint-augment M6 seam". Or keep the u32 field but make M5.2's sender add
per-type action-id readers (a real, un-budgeted work item). Also state the multi-model rows
explicitly: **B_TN has two arcs** (`dComIfG_resLoad(&mPhaseReq1, "B_tn")` + `mArcName`,
d_a_b_tn.cpp:5052) and **E_YC's wolf-bite/Midna is a separate E_RDY actor** resolved by
`fopAcM_SearchByID(mRiderID)` — the `{arc, model, anim}` row cannot represent either; the doc's
"crude boss limbs" note covers B_TN but not the missing rider.

### BLOCKER B3 — Per-material alpha override is not subtree-safe; shared J3DModelData
**Doc section:** §4.3 ("per-material alpha override (~100/255) on the ghost's J3D model").
**Problem:** enemy model data is **shared per arc** — `dComIfG_resLoad("E_AI")` caches one
`J3DModelData` in `dRes_control_c` (d_resorce.cpp), and every instance of that type (the
receiver's OWN real Armos in the same room, plus every ghost of that type) references the same
material objects. `J3DModelData` has **no clone API** in this tree. Calling
`material->setTevColor`/alpha on the ghost's modelData mutates the shared materials → the
receiver's real enemies render translucent too. This is not hypothetical: `d_kankyo.cpp` mutates
materials globally every frame precisely because modelData is shared.
**Fix (verified buildable in-tree):** per-instance override via `J3DMaterial::copy()` +
`J3DMatPacket::setMaterial()` per mesh (clone each material once per ghost, set alpha on the
clone, swap the packet's material; `J3DMaterial.h:33`, `J3DPacket.h:299`) — the standard
translucency trick — or reload the bmd binary for a private modelData (the d_resorce.cpp:378
warp-material re-load pattern). Either way §4.3 must say *cloned materials, never shared-model
mutation*, and the doc should budget memory: per-ghost clones = Σ(materials × 8-byte packets) per
type — small, but per-type clone-once-and-share-across-ghosts is the right granularity (clone the
type's materials once, stamp alpha once, share the J3DMaterial set among all ghosts of that type).

### MAJOR M1 — No receive-side stage gate; "cross-stage visibility" spawns ghosts in the wrong world
**Doc section:** §4.1 (broadcast own room), §4.3 (cull distance only), M5.3 acceptance
("cross-stage + same-stage visibility"), §7 ("cross-room/cross-stage ghost visibility").
**Problem:** GhostSnapshot carries **no stage field**, and §2 deletes the room-scoped fan-out in
favor of unconditional `SendToAll`. In parallel worlds, a sender in a *different stage*'s enemies
have coordinates that are garbage in the receiver's stage geometry — spawning them (unless
distance-culled by luck) renders enemy ghosts floating in the wrong world. The packet can't
distinguish; the receiver must track each sender's stage from their `PlayerState`
(`ReceiveSlot.state.stage`, exactly like the puppet `stageOk` gate) and spawn ghosts only for
`stage == myStage`. Then "cross-stage visibility" is a contradiction in terms — the testable
feature is **cross-room (same stage, adjacent rooms)**, where coords ARE meaningful. M5.3's
acceptance and §7's probe list say "cross-stage" and will test the wrong thing.
**Fix:** §4.3 adds: "ghost spawn gate: sender's tracked stage (from PlayerState) == local stage;
cross-room ghosts in the same stage are valid (coords are stage-local); cross-stage senders are
ignored entirely (no spawn, no TTL entry)". §7: rename the probe to "cross-room ghost
visibility".

### MAJOR M2 — `registerDynamicEnemy` "keep dormant" contradicts M5.4 projectiles
**Doc section:** §2 row ("keep dormant (waves are M6)") vs §4.4 (projectile ghosts) vs §4.2
("owner-major dynamic id ... for dynamic spawns").
**Problem:** every projectile the doc wants to ghost (arrow, boomerang, bomb, E_AI rock, E_FB
fire) is a **dynamic spawn** (`setID == 0xFFFF`), and M5.4's sender needs the dynamic id path
**live**. Worse, the "immediate scan + 15-frame backstop" cadence cannot discover projectiles at
all: they live < 15 frames (an arrow's flight is ~1 s, a rock shot ~0.5 s), so a 15-frame
registration backstop misses most of a projectile's life, and stage-placed enumeration
(`fopAcIt_Executor` over room actors with setID keys) doesn't apply. Projectiles need a
**per-frame transient scan + dynamic registration** on the sender — a new mechanism the doc
neither specifies nor reconciles with the "dormant" row.
**Fix:** rewrite the row: "`registerDynamicEnemy` + the owner-major id space: **kept, activated in
M5.4** for projectile/transient actors (M5.4 sender scans projectile profiles per frame,
registers under `DynamicEntityId(selfId(), counter)`); stage-placed enemies keep the
`(room<<8)|setID` space. Waves remain M6."

### MAJOR M3 — ALLDIE exclusion claim is mechanism-wrong: group, not registry
**Doc section:** §4.3 ("Ghosts never enter the local enemy registry, so ALLDIE/drops/story
switches ignore them completely").
**Problem:** ALLDIE does **not** consult the coop registry. `fopAcM_myRoomSearchEnemy`
(f_op_actor_mng.cpp:1897) scans the room's actor layer for **any live actor with
group == fopAc_ENEMY_e** (`enemySearchJugge`, :1887) — and it is used by `d_a_alldie.cpp`,
`d_a_door_shutter`, `d_a_door_spiral`, `d_a_tbox`. If `coop_ghost`'s profile registers group
`fopAc_ENEMY_e` (the *natural* choice for an enemy lookalike — the doc never states the group),
the receiver's doors **never open while any ghost is in the room** (the ALLDIE scan finds ghosts
forever). The claim holds only if the ghost profile's Group is explicitly non-enemy
(`fopAc_ACTOR_e`, like the ALLDIE profile itself), and even then it must be *tested*, because the
"invisible to AI" guarantee elsewhere in the doc is similarly registry-adjacent, not
actor-scan-adjacent.
**Fix:** §4.3 states the profile contract: "`coop_ghost` profile: Group = fopAc_ACTOR_e (NOT
fopAc_ENEMY_e), no fopAcStts status bits that draw enemy scans" + M5.3 selftest: spawn N ghosts in
a test room, assert `fopAcM_myRoomSearchEnemy(room)` still returns null and `d_a_alldie`'s
ACT_CHECK still transitions.

### MAJOR M4 — Arc residency across rooms/stages is unverified; §4.3 asserts the opposite
**Doc section:** §4.3 ("Models' arcs are stage-independent, so **cross-stage and cross-room
ghosts render identically**").
**Problem:** enemy arcs mount **per stage/room data** — `dComIfG_resLoad` registers an arc into
`dRes_control_c`, whose `getResInfoLoaded` reports "res nothing !!" for arcs the current stage
never mounted (d_resorce.cpp:890-897). A ghost of a room-B enemy type while we're in room A of
the same stage, or of any stage-B type, has **no guarantee the arc is resident**; the ghost's
create would stall or fail. The same-room case (the actual product use case) IS safe — both
machines mount the same room's arcs — but the doc's blanket "stage-independent" claim is
unverified. Related spawn-path concern (the doc asks it itself): first-sight on a full room
spawns 20-30 ghost creates in one frame, each an async resource phase — a spawn storm.
**Fix:** §4.3: "same-room ghosts reuse the receiver's already-resident modelData (the room's own
enemies loaded the same arcs); cross-room/cross-stage types go through an explicit
`dComIfG_resLoad`-style async load with a skip-on-fail fallback (unknown-type counter-log), and
per-frame spawns are paced (e.g. ≤ 8 ghost creates/frame)". M5.3's acceptance should include a
same-stage-adjacent-room case with a type absent from the receiver's room (expect: late or
skipped render, no crash).

### MAJOR M5 — §4.5 bandwidth framing is wrong twice; 8-player worst case not stated
**Doc section:** §4.5 ("≈ 62 KB/s per sender; 4-player shared room ≈ 185 KB/s aggregate — versus
the old ~1 MB/s worst case").
**Problem:** (a) the "~1 MB/s worst case" is **player pose traffic, which is unchanged** (2017 B ×
60 Hz × 8) — ghosts add on top of it, not instead of it; the honest comparison is old enemy layer
≈ 106 KB/s (40 × 44 B × 60, one owner stream) vs ghosts ≈ 62 KB/s **per sender**, and since
ghosts are broadcast from every machine the ghost layer is **O(players)**: 8 senders ≈ 496 KB/s
— *more* than the old owner-scoped enemy stream. (b) "185 KB/s aggregate" is actually per-receiver
*inbound* (3 remotes × 62); the 4-player wire aggregate is 248 KB/s. (c) the payload figure is
35 B, not 40 (B1), so all totals are ~12% high. The 8-player worst case (the prompt's question):
8 senders × (30×15 + 2×30 + 15×60) × 40 B ≈ **496 KB/s aggregate ≈ 0.5 MB/s**, on top of ~0.97
MB/s player pose ≈ 1.5 MB/s worst wire — still trivial on LAN, but state it.
**Fix:** rewrite §4.5 with (i) payload 35 B + 4 B envelope = 39 B, (ii) per-sender/per-receiver/
aggregate split, (iii) the 8-player table, (iv) the correct baseline comparison ("player pose
unchanged; enemy layer goes from one 106 KB/s owner stream to N × 62 KB/s sender streams").

### MAJOR M6 — Ghost sender hook point and render-table duties unspecified
**Doc section:** §4.2 ("PollOwnDeaths ... minus the switch/drop reads"), §4.3 (table = {arc,
model, anim}).
**Problem:** the old sender ran `hostOnExecuted` (f_op_actor.cpp:377), **owner-gated, post-
execute, whitelist-only**. The ghost sender must run **ungated on every machine, for every enemy
profile in the room** — a different hook contract the doc doesn't state (read pre-execute in
`onGameFrame`'s scan is acceptable — 1-frame-stale pose — but that's a decision, not a given).
Also: the render table spec {arc, model, anim} cannot serve M5.2's death detection — the old
`isDead` callbacks live in the deleted kAdapters, so the table needs a **deathTest entry**
(`health <= 0` per type / actor-gone backstop) or PollOwnDeaths re-implements per-type death
semantics for ~15 types twice.
**Fix:** §4.2 adds the sender hook contract (scan-point + per-frame pose read + who polls
deaths + registration gate widened from `isWhitelistedType` to "ghost-candidate"), and §4.3's
table gains a `deathTest` column. Registration gate: today `EligibleForRegistration` skips
non-whitelisted types AND `setID == 0xFFFF`; the ghost sender needs the opposite default.

### MINOR m1 — Dead surface the shred leaves behind (coop.h exports, stale comment)
`amIRoomOwner`, `remoteInRoom`, `puppetActorFor`, `resolveNearestPlayer` become dead after M5.1
(their only consumers are the deleted enemy/combat/context modules); the protocol.h bottom
comment promises a v7 that is not the ghost v7; the M4.5 capstone note in the same header ("M5
decides wire-vs-drop") becomes moot. Fix: M5.1 removes the exports, rewrites the version-history
block, and resolves the capstone "do NOT change behavior" notes as *dropped*.

### MINOR m2 — `EnemyEventMsg` keeps a redundant type field and an unused Spawned event
§4.2 keeps `type` and `Spawned=0 (advisory)` — but spawns are implicit-by-first-snapshot (the
receiver spawns on first sight; nothing consumes Spawned in v1) and `type` is redundant for the
`(senderId, entityId)` registry key. Trim v1 to `{u16 entityId, u8 eventId=Died}` (5 B); re-add
Spawned with M6 waves. Also `EnemyEventId::RoomClear/BossPhase` should be dropped from the enum in
the same commit.

### MINOR m3 — No scale in the packet, and scales demonstrably vary
The YC driveModel comment is proof per-instance scale varies (`l_HIO.mScale` file-static in the
actor TU; the frozen path re-extracted column magnitudes from the baseTRMtx to preserve it). A
fresh `coop_ghost` has no instance scale. Same-room ghosts can adopt the receiver's own
same-type actor's scale (same stage data ⇒ same setid/HIO — correct by construction unless a
player tweaks HIO), which is fine, but must be stated as the v1 mechanism + the HIO-divergence
caveat.

### MINOR m4 — 15 Hz enemies vs projectiles' speed-advance: inconsistent + no interpolation
§4.2 applies packets as-is for enemies but §4.4 speed-lerps projectiles; at 15 Hz with no
interpolation, fast lunges (B_TN is at 30 Hz, but E_DF dives and E_AI stabs are 15) teleport up
to ~66 ms per step — documented as risk 5, but the cheap fix (per-row cadence: walkers 15 Hz,
lungers 30 Hz — +20 KB/s/sender worst) belongs in the doc, not the M5.5 polish backlog.

### MINOR m5 — Ring overflow is a non-risk; say so
kSnapshotRingCapacity = 128 slots; worst ghost burst (30 enemies/15 Hz + 2 bosses/30 + 15
projectiles/60 ≈ 18 pkts/frame ≈ 39 B each) leaves > 100 frames of headroom, and replace-newest
handles pressure by design. No finding — just answer the question the doc should have answered.

### MINOR m6 — "TTL covers sender stage-switches" is misleading phrasing
A stage switch stops the sender's loop for > 1 s, so the TTL *converts* the switch into a full
despawn + respawn (post-load immediate scan re-announces ~1 frame later). That's the intended
behavior, but §4.2's wording implies continuity. Say: "sender loads/room changes pop ghosts for
up to ~1 s + load time; accepted".

### MINOR m7 — M5.0 acceptance is undefined
M5.0 says "this doc through the adversarial pair; fixes folded in, then committed" — no
deliverable definition. State: adversarial review file committed + §2 shred checklist verified by
grep over the attachment table + protocol.h v7 compiles + forced rebuild green with ghosts
stubbed (M5.1's own acceptance already covers the last two).

### MINOR m8 — `net.enabled=false` vanilla guarantee needs a M5.1 regression row
TESTING.md's "net-off boot untouched" is the standing contract, but with the shred removing
d_cc_s.cpp's only coop hook, the guarantee now also covers "no ghost code runs when the session is
off" — add a selftest/boot row to M5.1's acceptance (off-session: zero ghost sends, zero registry
entries, `myRoomSearchEnemy` unaffected).

---

## 4. RISK FRAMING — is §6 the right five?

The five listed are real but miss the two that the live run will actually show:

1. **Ghost crowd vs room actor budget (missing).** A shared room with 4 players: 30 real enemies
   + 3 remotes × 24 ghosts = **~100 actors in one room** (8 players: 168 ghosts). TP rooms were
   built for ~10-30; the draw/mtx cost and the first-sight create storm (M4) are the load
   hazards, not bandwidth. Add as risk 6.
2. **Anim-fidelity collapse (missing, and it's the worst live-show).** B2 means every ghost of a
   type plays one bck — the single most visible artifact of the whole pivot; the doc's risk 3
   covers only *boss limbs*. Fold B2's v1 statement into risks.
3. Risk 6 ("puppets invulnerable to ghosts") is right and should also note ghosts have **no Co**
   collider either — Link walks through a ghosted Darknut with zero feedback (consistent, but a
   first-play surprise worth one line).
4. The ALLDIE-door hazard (M3) belongs in §6 — it's a *blocked gameplay* risk if the group is
   wrong, not a polish item.

**Worst thing a live two-instance run shows (§7 should pre-call these):** (a) every ghost of a
type animates as one synchronized bck — fight scenes look like cardboard cutouts in march;
(b) on the sender's first room entry the receiver hitches on a 20-30-ghost create storm; (c) a
sender opening the pause menu / crossing a load pops all their ghosts after 1 s and they respawn
late; (d) E_YC ghosts fight invisible Midna; (e) projectiles ghost through walls (speed-lerp has
no collision) — accepted, but state it in §7 so the playtest records it as expected.

## 5. MILESTONE SHAPE

- **M5.1 SHRED first: right call.** Risk-front-loading is correct — the pivot's only proven
  asset is the surviving core, and a forced rebuild + green selftest with ghosts stubbed proves
  it stands alone before any rendering. Add the grep-driven shred checklist (my §2 table) to its
  acceptance.
- **M5.3 is the one to split.** It bundles the registry lifecycle (safe, table-testable) with the
  render table + morf construction + alpha clones (the risky part, B2/B3). Split: **M5.3a**
  registry + spawn/apply/despawn/TTL/cap with a placeholder box model (proves lifecycle + M3's
  ALLDIE-group check + stage gate), **M5.3b** per-type render rows + morf construction + cloned-
  material alpha (proves the std-code claims). M5.3a's selftest is the one the doc already
  describes; M5.3b gets the per-type visual acceptance.
- **M5.4 projectiles: right timing, wrong prerequisites.** It's correctly last (60 Hz + lerp is
  the fiddly math), but it cannot start until the M2 contradiction is resolved (dynamic id path
  live + per-frame transient scan). Make that an explicit M5.4 step 0.
- **M5.0 acceptance (doc-only is fine):** review file committed, §2 table corrected per this
  review, no code.

## 6. OVERSTATED-FEASIBILITY AUDIT ("buildable" claims vs the std code)

| Doc claim | Reality |
|---|---|
| "per-material alpha override on the ghost's J3D model" | **Overstated.** Shared modelData — see B3; fix exists (material copy + packet swap) but is not what's written |
| "anim-apply callback ... like the frozen-puppet path" | **Overstated.** Frozen path had a live actor state machine; ghost has none, and action ids are never on the wire — see B2 |
| "action id u16 \| frame u16" (anim field) | **False today.** PackAnim emits frame-only; high half is wire-zero since v4 |
| "Models' arcs are stage-independent, so cross-stage and cross-room ghosts render identically" | **Unverified.** Per-stage/room arc mounting; same-room is safe, cross-room/stage is not — see M4 |
| "cross-stage ... ghost visibility" (M5.3 acceptance, §7) | **Wrong test.** Cross-stage ghosts are meaningless in parallel worlds — see M1 |
| "GhostSnapshot is ~40 B" | **Off by 5.** 35 B packed (36 with the missing senderId) — see B1 |
| "62 KB/s per sender ... vs the old ~1 MB/s" | **Misleading framing.** Pose traffic unchanged; ghost layer is O(players) — see M5 |
| "Ghosts never enter the local enemy registry, so ALLDIE ... ignore them" | **Mechanism-wrong.** ALLDIE scans actors by group, not the registry — see M3 |
| "per-sender ghost cap 24, spawn-side reject" | Fine — but count projectiles against it (or a second cap) and pace spawns per frame (M4) |

## 7. MUST-FIX (before M5.1 code)

1. **B1** — add `senderId` to GhostSnapshotMsg; fix the 35/36-B wire math; drop self-echo on the
   host.
2. **B2** — rewrite the anim contract honestly (v1 = one bck per type, frame-only; defer action
   ids to M6); add multi-model rows (B_TN two arcs, E_YC rider) to the acceptance.
3. **B3** — specify cloned per-instance materials (J3DMaterial::copy + J3DMatPacket::setMaterial),
   never shared-modelData mutation.
4. **M1** — receive-side stage gate keyed on the sender's tracked PlayerState stage; §7 probe
   renamed to cross-room.
5. **M3** — profile Group = fopAc_ACTOR_e contract + M5.3 ALLDIE selftest.
6. **M2** — reconcile the dormant row with M5.4 (dynamic ids + per-frame transient scan).

## 8. NICE-TO-HAVE

- Shred-table corrections: drop `coop_drops.cpp` (→ coop_enemy.cpp), drop `d_cc_uty.cpp`, name the
  f_op_actor.cpp :357/:377 blocks, list the dead coop.h exports + the stale v7 comment.
- §4.5 rewrite per M5 (35-B payload, per-sender/per-receiver/aggregate, 8-player row, honest
  baseline).
- Per-row cadence (30 Hz lungers) instead of flat 15 Hz — 20 KB/s/sender.
- Trim EnemyEvent to `{entityId, Died}` (drop Spawned/type) — 6 → 5 B.
- Scale v1 mechanism (receiver adopts local same-type actor's scale) + HIO caveat.
- §6 add: ghost-crowd room budget, anim-fidelity collapse, no-Co ghost pass-through note.
- M5.1 acceptance: net-off boot row (m8) + grep checklist.
- Spawn storm pacing (≤ 8 ghost creates/frame) + same-room modelData reuse.

---

*Review artifacts: all file/line references verified on branch `net-coop` @ 7691015066. No source
files modified.*
