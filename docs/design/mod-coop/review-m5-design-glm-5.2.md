# Adversarial design review — M5 pivot (05-ghosts.md)

Reviewer: glm-5.2 · Branch: `net-coop` · Scope: 05-ghosts.md (DESIGN) traced
against the real `net-coop` tree (protocol.h, session.cpp, coop_enemy.cpp,
coop_combat.cpp, coop.cpp, f_op_actor.cpp, d_a_alldie.cpp, d_cc_s.cpp,
d_cc_uty.cpp, d_a_e_yc/ai.cpp, d_a_player.h, f_op_actor_mng.{h,cpp},
d_attention.cpp, transport.h, selftest_main.cpp, config).

## VERDICT

**The pivot is sound and the shred is *mostly* complete, but the ghost
receiver is NOT buildable as documented against the real code.** Three
things in §4 are overstated:

1. The "slim per-type anim-apply callback" (§4.3) drastically understates
   the per-type model-init work a generic `coop_ghost` actor must do — the
   existing `driveModel` callbacks lean on each enemy's `create()` having
   already built the morf from the right arc/bck, which a generic ghost
   does not have. (Finding F3, MAJOR)
2. The "per-material alpha override" (§4.3) is unsafe against TP's shared
   `J3DModelData` resource cache — it will turn every real enemy of the
   same type translucent alongside the ghost. (Finding F2, MAJOR)
3. "Cross-stage and cross-room ghosts render identically" (§4.3) conflates
   model loading with positional coherence. There is no receive-side
   same-stage gate, the packet carries no stage field, and `entityId`'s
   room byte is not unique across stages — so a broadcast ghost from
   another stage spawns at garbage coordinates. (Finding F1, BLOCKER)

The §2 shred list catches the big surfaces but has one wrong file
(d_cc_uty.cpp) and under-specifies the session-layer deletion boundary
(`playerRoom_[]` vs `g_receive`). The §6 risk list misses the single
most likely live-playtest eyesore (fast-enemy ghost stutter at 15 Hz with
no position lerp) and the d_attention lock-on leak.

**Ship-readiness:** approve M5.0 after the MUST-FIX list is folded. Do
NOT approve M5.3 (receiver) acceptance on the current §4.3 text.

---

## RANKED FINDINGS

### F1 — BLOCKER — No same-stage/same-room receive gate; cross-stage ghosts spawn at garbage coordinates
**§4.1, §4.2(receiver), §4.3.** The packet carries `entityId` (whose
`(room<<8)|setID` room byte is explicitly *not* unique across stages —
see protocol.h's PlayerState `stage` comment: "room numbers are not
unique across stages (spring / house interiors)") and `pos` in the
sender's room-local coordinates, but **no stage field**. §4.2 receiver
says "First sight → spawn" with no gate. §4.3 asserts "Models' arcs are
stage-independent, so cross-stage and cross-room ghosts render
identically" — true for the *model*, false for the *position*: a sender
in `F_SP103` room 1 broadcasting to a receiver in `F_SP108` room 1
produces a ghost keyed `(sender, 0x0103)` rendered at `F_SP103`-room-1
coordinates inside the receiver's `F_SP108` world. The §4.5 bandwidth
math implicitly assumes a "shared room," but the wire design is
broadcast-to-all with no receive filter.

This also blows up the arc residency model: a receiver in a stage without
E_YC would have to `dComIfG_resLoad("E_yc")` a foreign arc (memory +
lifecycle the doc doesn't own).

**Fix text for §4.2 receiver:**
> A ghost is spawned only when the sender is confirmed to share the
> receiver's stage AND room. The gate cross-references the PlayerState
> stream already maintained by the puppet layer:
> `g_receive[sender].hasState && strcmp(g_receive[sender].state.stage,
> localStage)==0 && g_receive[sender].state.roomNo == localRoomNo`.
> Snapshots from a sender whose `g_receive` slot has no state, or is in a
> different stage/room, are counted and dropped (never spawned). This
> keeps ghosts geographically coherent, reuses the puppet layer's room
> tracking (no new stage field on the wire), and guarantees the ghost's
> arc is already resident on the receiver (same stage ⇒ same enemy
> population ⇒ arc already loaded by the stage). §4.3's "cross-stage
> ghosts render identically" is reworded to "cross-stage ghosts are NOT
> rendered — the receive gate drops them; cross-room ghosts within the
> same stage are the only cross-room case."

### F2 — MAJOR — Per-material alpha override mutates shared J3DModelData
**§4.3 Render.** TP enemy morfs are built from the stage resource cache,
e.g. `dComIfG_getObjectRes("E_yc", 24)` returns a `J3DModelData*` shared
by every E_YC instance in the stage (verified in d_a_e_yc.cpp:776). The
`J3DMaterial` objects live *inside* that shared `J3DModelData`. A
"per-material alpha override (~100/255) on the ghost's J3D model"
mutates the shared material state, so **every real E_YC in the room
goes translucent alongside the ghost.** The doc's "standard TP
translucent draw (PSTranslucent + material alpha)" is not a safe recipe
here — there is no existing translucent-actor precedent in the coop code
(puppets are *hidden* via `isPuppet` guards, not alpha'd; verified — no
setMaterialAlpha/PSTranslucent pattern in d_a_player.cpp / d_a_alink.cpp
/ coop_*). 

**Fix:** §4.3 must pick one and verify the primitive exists:
- (a) **Clone `J3DModelData` per ghost** (deep-copy materials so alpha
  writes are local) — safe but costs heap per ghost (see F11), or
- (b) **Non-mutating per-draw alpha** — set the alpha at draw time into a
  per-model register / tev color that does not write back into shared
  material state (e.g. a `J3DMaterial`-local override flag + a draw-time
  `GXSetTevColor`/alpha-reg write, or a copy-on-first-write material).
  Preferred — no per-ghost model-data clone.

The doc currently names neither. M5.3 acceptance must include a selftest
(or live check) that a ghost and a real same-type enemy co-render with
the real one **opaque**.

### F3 — MAJOR — The "slim per-type anim-apply callback" understates the receiver model-init
**§4.3 Pose, §4.4.** The existing `driveModel` callbacks (coop_enemy.cpp
`aiDriveModel`/`ycDriveModel`/`mdDriveModel`/`tnDriveModel` …) work by
casting `fopAc_ac_c*` to the **concrete enemy type** and calling PUBLIC
methods (`setBaseMtx()`, `mpMorf->setFrame()`, `mpMorf->modelCalc()`)
on an actor whose own `create()` already:
- `dComIfG_resLoad`'d the type's arc,
- built the `mDoExt_McaMorfSO` from `dComIfG_getObjectRes("E_yc", 24)`
  with the correct bck/vaf,
- wired `fopAcM_SetMtx` to the model's baseTRMtx.

A **generic `coop_ghost`** actor has none of that. Per ghost, the
receiver must: resLoad the arc, instantiate `J3DModelData`, build the
morf (or `J3DModel`+`J3DAnmChr`) with the right bck, then drive frame.
That is **per-type model-init**, not "the slim survivor of driveModel."
The §5 milestone "render table `{procName → arc, model name, anim name}`"
is the right shape but the doc hides the build cost behind it.

**Fix:** §4.3 must state honestly that the render row is
`{procName, arcName, modelDataResId, bckResId, morfKind
(McaMorfSO|J3DModel+J3DAnmChr), buildFn, frameFn}` and that `buildFn`
replicates the per-type model construction each enemy's `create()` does
(minus colliders/AI). Alternatively, commit to **one ghost actor class
per seeded type** (subclass the real enemy, neuter `execute`/colliders,
reuse `create`/`draw`) — heavier code, but reuses the real create/draw
verbatim. The doc must pick a strategy before M5.3 is sized.

### F4 — MAJOR — coop_ghost profile group vs `fopAcM_myRoomSearchEnemy` + d_attention
**§4.3 Interaction.** The doc says "Ghosts never enter the local enemy
registry, so ALLDIE/drops/story switches ignore them completely." But
ALLDIE doesn't scan the coop registry — it scans the **fopAc actor
manager** via `fopAcM_myRoomSearchEnemy`, which filters
`fopAcM_GetGroup(actor) == fopAc_ENEMY_e` (verified,
f_op_actor_mng.cpp:1882 `enemySearchJugge`). The same predicate is used
by `d_a_door_shutter`, `d_a_door_spiral`, `d_a_tbox`,
`d_a_obj_lv4CandleDemoTag`. If `coop_ghost`'s profile sets
`Group = fopAc_ENEMY_e` (the obvious choice for an "enemy mirror"), **doors
never open, chests never unlock, candle demos never fire** in any room
containing a ghost.

Symmetric problem on the player side: `d_attention.cpp` (line 655) has a
coop guard that excludes *puppets* from the lock-on/check lists. There is
**no such guard for `coop_ghost`** in the doc. Without one, the player
can Z-target a ghost (meaningless — it has no colliders/hit), and the
camera's CheckObjectTarget list can include ghosts.

**Fix:** §4.3 must specify the `coop_ghost` profile:
- `Group` ≠ `fopAc_ENEMY_e` (use `fopAc_ACTOR_e` or a dedicated group),
  AND status bits that exclude it from `enemySearchJugge` and any other
  enemy-search predicate;
- a d_attention exclusion: "a `coop_ghost` actor is never added to any
  attention target list" — same shape as the existing puppet exclusion
  (d_attention.cpp:655), gated on procName == `coop_ghost` (or a
  `isGhost()` predicate).

M5.3 selftest must assert: a ghost in a room with an ALLDIE does NOT
block ACT_TIMER, and a ghost is not returned by the attention lock-on
scan.

### F5 — MAJOR — GhostSnapshot wire size is 35 B, not 40 B
**§4.2, §4.5, M5.2 acceptance.** The struct sums to
`u16(2)+u16(2)+u8(1)+s16(2)+u32(4)+Vec3f(12)+Vec3f(12) = 35 B`. The
doc's comment `// 5+4+1+2+4+12+12 = 40 B` is garbled arithmetic that
does not map to the fields and sums wrong. The protocol uses
hand-serialized `ByteWriter` with **no padding** (verified — every field
written individually, no struct padding, no alignment), so 35 B is the
real wire size. §3 says `WireSize` is updated for v7; M5.2 acceptance
says "wire 40 B asserted" — **that selftest will fail.** §4.5 bandwidth
math is ~14% off (uses 40).

**Fix:** recompute to 35 B payload (+4 B envelope = 39 B). Update §4.2
comment, §4.5 (per-sender → (30×15+2×30+15×60)×35 ≈ 49 KB/s; 4-player
shared room ≈ 162 KB/s), and M5.2's asserted size.

### F6 — MAJOR — §4.2 PollOwnDeaths contradicts the §2 adapter deletion
**§2 (kAdapters/isDead/deathSwitchNo deleted), §4.2 ("the adapter-on-
entry UAF fix … carries over, minus the switch/drop reads").** The M4.6
UAF fix *is* the adapter capture — "store `s16 procName` + death-test
callback at registration, never deref after `gone`"
(implementation-plan.md M4.6 MAJOR 2). With `kAdapters`, `isDead`, and
`deathSwitchNo` deleted in §2, **there is no death-test callback to
capture.** The per-type `isDead` callbacks encoded real differences
(`hmIsDead`: `health<=0`; `aiIsDead`: always-false, actor-deletion
backstop; `ycIsDead`: `health<=0` but wolf-bite-only). A generic
post-shred death test ("actor gone from registry") loses the **death
window** (health≤0 but actor not yet deleted) — the `Died` event fires
late or only on actor deletion, and the ghost lingers into the death
anim then pops.

**Fix:** §4.2 must specify the post-shred death test explicitly. Minimal
honest option: keep a **sender-only slim table**
`{procName, bool(*isDead)(const fopAc_ac_c*)}` (NOT the full
`NetEnemyAdapter` — no injectHit, no semantics, no dropTableId), plus
the existing actor-deletion backstop. The §2 row should say "delete the
combat/switch/drop adapter fields; keep a sender-only `{procName, isDead}`
slim table for PollOwnDeaths."

### F7 — MAJOR — 8-player bandwidth reverses the "vs old 1 MB/s" win + snapshot-ring overflow
**§4.5, §6.** §4.5 computes the 4-player case and claims "versus the old
~1 MB/s worst case." Ghost-layer bandwidth scales **linearly with player
count** in a shared room (N senders each broadcast the same room's
contents). At 8 players in one room:
- ghosts: 8 × (30×15 + 2×30 + 15×60) × 35 B ≈ 8 × 49 KB/s ≈ **395 KB/s**,
- PlayerState: 8 × 2017 B × 60 Hz ≈ **966 KB/s**,
- aggregate ≈ **1.36 MB/s — exceeding the old worst case the doc claims
  to beat.**

Worse on the hot path: the inbound snapshot ring
(`kSnapshotRingCapacity = 128`, transport.h:60) is a **single SPSC** fed
by all peers (one Transport, star topology). 8 senders × ~24 ghost
msgs/frame > 128 ⇒ **systematic overflow** and ghost dropouts, well
before the 1 s TTL can mask loss. §6 risk 5 mentions TTL covering packet
loss but not ring overflow.

**Fix:**
- §4.5: restate bandwidth as linear in N; acknowledge 8-player rivals
  the old model; give the 8-player number.
- §6: add a risk "snapshot-ring overflow at ≥5 players in a shared room
  with projectiles — raise `kSnapshotRingCapacity` or cap per-sender
  ghost counts / drop projectile cadence to 30 Hz for >4 players."
- M5.5 polish should include a telemetry row for snapshot-ring drop
  count, not just bandwidth.

### F8 — MAJOR — d_cc_uty.cpp is mis-listed in the §2 combat row
**§2 combat row** lists `d_cc_uty.cpp` alongside `d_cc_s.cpp` for
"revert d_cc intercepts to vanilla `#else` paths or remove the hooks."
Verified: d_cc_uty.cpp's `TARGET_PC` blocks are
`dusk::getSettings().game.invincibleEnemies` (a settings cheat) and
`dusk::AchievementSystem::signal("enemy_killed"/"rollstab_kill")` —
**fork features, not coop combat intercepts.** There is no combat hook
there to revert; "reverting" would delete unrelated fork features.
The coop combat intercept lives **only** in d_cc_s.cpp:547
(`dusk::coop::combat::noteAtTgHit`).

**Fix:** remove `d_cc_uty.cpp` from the combat-shred row. The row should
read `protocol.h, coop_combat.cpp, coop_enemy.cpp, d_cc_s.cpp` only.
Keep d_cc_uty.cpp's blocks intact.

### F9 — MAJOR (clarity) — §6 misses the worst live-playtest eyesore: fast-enemy ghost stutter
**§6 risk 5, §7.** §4.4 lerps **projectiles** by `speed` between packets,
but **enemy/boss ghosts get pose+anim snap at 15 Hz with no position
lerp** (the doc only promises projectile lerp). A fast E_YC kargorok or
E_AI knight ghost at 15 Hz = a 66 ms position snap = a stop-motion
slideshow teleporting through the player's own solid copy. §7 probes
"ghost spawn/despawn" and "cull distance" but **not fast-enemy stutter.**
This is the single most visible thing a live two-instance run will show
in any shared room with fast enemies.

**Fix:** §6 add risk "fast-enemy ghosts at 15 Hz visibly stutter
(position snap, no lerp) — pre-commit to either (a) 30 Hz for fast types
(E_YC/E_AI/E_HM knights and flyers) or (b) receiver-side position lerp
for ALL ghosts (not just projectiles), extrapolating by `speed` like
projectiles do." §7 playtest add "fast-enemy ghost smoothness in a
shared room (kargorok/nightmare) — accept or bump cadence."

### F10 — MAJOR — Host-leave / client-leave ghost cleanup not in §6
**§4.2 ("Sender leaves / session ends → clear all their ghosts"), §6.**
The doc states the cleanup but §6 doesn't list it as a risk. The hidden
edge: a **non-host client** leaving mid-ghost. The host broadcasts
`PlayerLeave`; every receiver must clear that sender's slice of the
`GhostRegistry`. If the `PlayerLeave` reliable packet is lost (it's on
channel 0, so ordered/reliable — but a host crash drops it), the
receiver's `g_receive[sender]` goes stale and the §4.2 receive gate
(F1) would still spawn ghosts from a buffered snapshot. The 1 s TTL
covers it, but the doc should say so.

**Fix:** §6 add "ghost cleanup on sender leave relies on the reliable
`PlayerLeave` (host-crash path) + the 1 s TTL as backstop — a sender
whose `PlayerLeave` is lost clears on TTL expiry, not instantly.
`GhostRegistry` must key by `senderId` and offer a `clearSender(pid)`
called from the `PlayerLeave` handler."

### F11 — MINOR — `anim` frame is u16 integer; no anim lerp
**§4.2 `anim`.** `u32 anim = action id u16 | frame u16`. The existing
sender packs `morf->getFrame()` into `u16` (truncating the float,
coop_enemy.cpp:55 etc.). At 15 Hz you can lerp **position** but not the
**anim frame** — ghost anims snap per packet. §6 risk 5 says "ghost
motion is choppier" but conflates position chop with anim-frame
quantization.

**Fix:** either widen the frame (e.g. `action u8 | frame Q24.8` fixed-
point in the u32) and have the receiver advance frame by `dt ×
animRate` between packets, or explicitly accept anim snap in §6 and
stop claiming lerp helps enemies (it only helps projectiles).

### F12 — MINOR — `flags` dead-gone bit is redundant with Died + TTL
**§4.2.** Despawn triggers are "Died event OR 1 s silence TTL" (§4.2
receiver) AND `flags` has a "dead-gone bit." Two mechanisms for the same
event. Pick one (prefer Died+TTL; drop the bit, repurpose the byte for
F4's projectile/fast-type bits or a form bit).

### F13 — MINOR — `Spawned` event is dead traffic
**§4.2 EnemyEventMsg.** Receiver spawns on first snapshot ("First sight
→ spawn"); `Spawned=0` is "advisory" with no consumer named. Either drop
it (save a reliable message + a selftest row) or justify it — the only
honest justification is **reliable arc preload before the first 15 Hz
snapshot** to avoid a 1-frame pop / load hitch. The doc does neither.

### F14 — MINOR — `registerDynamicEnemy` is "kept dormant" but M5.2 actively uses it
**§2 last row, §4.1.** §2 says "keep dormant (waves are M6)." §4.1 says
dynamic spawns get owner-major ids "the existing scheme in
`coop_entity_logic.h`." The M5.2 **sender** needs `registerDynamicEnemy`
NOW to assign dynamic-spawn ghost ids (a script-spawned Bokoblin is a
ghost source on day one, not an M6 wave). 

**Fix:** reword §2 row to "keep and repurpose — the sender uses
`registerDynamicEnemy` to assign dynamic ghost ids; the id scheme is
unchanged."

### F15 — MINOR — Session-layer deletion boundary under-specified
**§2 rows 1–2.** `playerRoom_[]` + `UpdatePlayerRoom` are listed for
deletion. I verified the surviving PlayerState sender gate
(`RemoteInOurRoom`, coop.cpp:418) reads **`g_receive[].state`** (the
received PlayerState stream), NOT `playerRoom_[]` — so the gate survives
the deletion. But a reader of §2 cannot tell that. The doc must state
the session survival boundary in one sentence:

> Session survives with: `roster_`, `worldStage_`, the per-receiver
> `g_receive[].state` room tracking (coop.cpp). Session loses:
> `playerRoom_[]`, `ownership_`/`RoomOwnershipTable`, `UpdatePlayerRoom`,
> `SendToAllInRoom`, `RouteCombatIntent`, the `RelayPolicy::RoomScoped`
> branch of `ForwardGameMessage`/`SendGameMessage`, and `PolicyFor`'s
> RoomScoped/None combat cases. Ghosts bypass session room-scoping
> entirely (broadcast `SendToAll`); the §4.2 receive gate (F1) is the
> only room filter.

### F16 — MINOR — fopAcM_create ghost overhead at 8 players
**§4.3.** 24 ghosts/sender × 8 = **192 extra fopAc actors** in the
actor manager's execute/draw iteration, each spawned via the full
`fopAcM_create` pipeline (heap alloc, `cPhs_INIT`→`cPhs_COMPLEATE`
phases). Bounded but non-trivial, and the doc raises "a lighter non-
fopAc render object better?" in the attack vector without answering.
`fopAcM_create` buys drawlist integration for free; a non-fopAc render
object needs custom drawlist insertion. 

**Fix:** §4.3 note the actor-count cost (cap × player count) and commit
to `fopAcM_create` (justified: free drawlist/cull integration) OR to a
lighter render object. M5.5 telemetry should log live ghost actor count.

### F17 — MINOR / DOC GAP — §8 gaps the M5 developer needs
The §8 "M6 seams" list is fine, but the doc omits several things the
M5.3 developer needs on day one:

- **No per-type render-table seeding LIST.** §4.3 says "seed rows: E_AI,
  E_HM, E_DF, E_YC, E_MD, B_TN + the boss profile list + common stage
  enemy types" but never enumerates the boss profile list, the common
  stage types, or the projectile types beyond 5 examples. M5.3 needs an
  actual table with `{procName, arcName, modelDataResId, bckResId,
  morfKind}` per row (and the projectile seed list from §4.4).
- **No `net.enabled=false` vanilla guarantee for the ghost layer.** §7
  playtests "save mtimes untouched" but the design must state: with
  `net.enabled=false` (or no live session), the ghost sender enumerates
  nothing, the receiver spawns nothing, and the tree is byte-for-byte
  vanilla single-player. (The existing `isPuppet`/sender gates already
  do this for puppets; the ghost layer must match.)
- **No NOTIFY/toast behavior on ghost presence.** Does the HUD tell the
  player "spectating X's fight"? Silent is fine, but the doc should say.
- **No ghost × puppet coexistence rule.** A sender's puppet AND their
  ghosts co-render on a receiver. The puppet's hidden gate
  (`stageOk`/`sameStage && roomNo==local`) and the ghost's receive gate
  (F1) use the *same* `g_receive[sender].state` — confirm they agree (a
  sender visible as a puppet is also a valid ghost source; a sender
  hidden as a puppet should NOT spawn ghosts — the F1 gate already
  handles this because it keys on the same state). State this so a
  future editor doesn't diverge them.
- **No ghost registry memory bound** beyond the per-sender cap (24).
  Add a budget: `modelDataSize × cap × playerCount` (and note F2's clone
  cost if option (a) is taken).

### F18 — MILESTONE SHAPE — M5.0/M5.1/M5.4 notes
- **M5.0 acceptance is doc-only** — correct, but the doc never states
  the acceptance line. Add: "M5.0 accept = this doc + the §2 shred list
  + the §4 receiver plan pass adversarial review; BLOCKER/MAJOR fixes
  folded; commit."
- **M5.1 SHRED first is the right call** (risk-front-loaded: prove the
  simplified core builds green with ghosts stubbed). But "selftest
  rewritten" must NAME the suites: drop `RunM2RelayPolicyCheck`,
  `RunM4OwnershipTableCheck`, `RunM4RoomRoutingCheck`,
  `RunM4EntityStabilityCheck`, and the ownership parts of
  `RunM4WorldStageCheck`; keep `RunProtocolChecks`,
  `RunDeterminismChecks` (extended to the v7 type set — GhostSnapshot +
  trimmed EnemyEvent), `RunHandshakeDemo`, `RunM3TimeWeatherCheck`,
  `RunM46SessionRestartCheck`, `RunM4DiscoveryCheck`. Add a v7
  `WireSize`/round-trip row for GhostSnapshot (35 B, per F5).
- **M5.4 projectiles** is correctly placed (after M5.3 — it reuses the
  generic ghost actor + seed rows + speed lerp). Not too late. But
  projectiles are the worst ring-overflow offender (60 Hz × 15 × 8
  senders, F7); M5.4 must explicitly cap projectile ghost count or drop
  to 30 Hz for >4 players, and its selftest should assert the
  snapshot-ring drop counter stays 0 under burst.

---

## MUST-FIX (before M5.0 approve)
1. **F1** — add the same-stage/same-room receive gate to §4.2; reword
   §4.3's cross-stage claim.
2. **F2** — pick a safe alpha mechanism (clone vs non-mutating per-draw)
   and specify it in §4.3; add the "real enemy stays opaque" check to
   M5.3 acceptance.
3. **F3** — honestly size the per-type model-init (render row shape +
   buildFn) OR commit to per-type ghost classes; update §4.3/§5.
4. **F4** — specify `coop_ghost` profile group ≠ `fopAc_ENEMY_e` and the
   d_attention exclusion; add the ALLDIE/lock-on selftest rows.
5. **F5** — fix the wire size to 35 B in §4.2/§4.5/M5.2.
6. **F6** — specify the post-shred sender death test (slim
   `{procName,isDead}` table or generic test); reconcile §2 and §4.2.
7. **F8** — remove d_cc_uty.cpp from the §2 combat row.

## NICE-TO-HAVE (fold during M5.0 or at M5.5)
- **F7** — 8-player bandwidth + ring-overflow risk in §6; raise
  `kSnapshotRingCapacity` or cadence-cap for >4 players.
- **F9** — fast-enemy 15 Hz stutter risk in §6; pre-commit to 30 Hz fast
  types or position lerp for all ghosts; add the §7 playtest row.
- **F10** — sender-leave ghost cleanup risk in §6; `clearSender(pid)` on
  `PlayerLeave`.
- **F11** — anim-frame quantization (widen or accept).
- **F12** — drop the redundant `flags` dead-gone bit.
- **F13** — drop or justify `Spawned`.
- **F14** — reword `registerDynamicEnemy` as "keep and repurpose."
- **F15** — one-sentence session survival boundary in §2.
- **F16** — note fopAcM_create actor-count cost; commit to a path.
- **F17** — the §8 day-one gaps (seeding list, `net.enabled=false`
  guarantee, toast behavior, ghost×puppet coexistence, memory bound).
- **F18** — M5.0 acceptance line; M5.1 suite name list; M5.4 ring-overflow
  cap.

---

## Where the doc overstates feasibility

- **"Cross-stage and cross-room ghosts render identically" (§4.3)** —
  false for position. Cross-stage ghosts must be DROPPED by a receive
  gate, not rendered. (F1)
- **"per-material alpha override … standard TP translucent draw" (§4.3)**
  — not safe against shared `J3DModelData`; no existing precedent in the
  coop tree. (F2)
- **"a per-type anim-apply callback (the slim survivor of driveModel)"
  (§4.3)** — the survivor needs a full per-type model-init, not a slim
  callback. (F3)
- **"the adapter-on-entry UAF fix carries over, minus the switch/drop
  reads" (§4.2)** — the fix *is* the adapter capture; with adapters
  deleted there is nothing to carry. (F6)
- **"versus the old ~1 MB/s worst case" (§4.5)** — reverses at 8 players
  (≈1.36 MB/s aggregate). (F7)
- **"ALLDIE/drops/story switches ignore them completely" (§4.3)** — true
  only if the `coop_ghost` profile group is set correctly, which the doc
  never specifies. (F4)
- **"GhostSnapshot is ~40 B" (§3/§4.2)** — 35 B. (F5)

The pivot itself — parallel worlds + ghost spectating, delete the
authority stack — is the right call and the §2 shred is ~90% complete.
The receiver is the weak half of the doc and needs the F1–F4 rework
before M5.3 can be honestly estimated.
