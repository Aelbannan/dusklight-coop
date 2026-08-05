# Investigation 02 — Player state sync surface

Status: **Investigation report. Revised after the SoH Anchor reference (full-pose
sync). Feed into the consolidated implementation plan.**

Scope: what per-frame state of a local `daAlink_c` must be read and transmitted so
a remote machine can faithfully render that player as a puppet, and how each piece
is read **from a stock (unmodded) Dusklight binary via the mod SDK**.

Target: **STOCK Dusklight** — no fork-only symbols. Everything below is read
through public game headers (`include/`), public fields/methods, or symbol hooks.
No `src/dusk/coop/` dependency survives in the mod.

---

## 0. Reference: SoH Anchor pose sync (why this investigation changed)

Ship of Harkinian's "Anchor" co-op (`soh/soh/Network/Anchor/`) solves the same
player-state-sync problem in a different decomp (OoT) with a proven approach:

- **`PLAYER_UPDATE` is sent every frame** from an `OnPlayerUpdate` hook
  (`HookHandlers.cpp:90`), only when another client is in the same scene and has
  a save loaded (`PlayerUpdate.cpp`, `currentPlayerCount == 0 → return`).
- **The full pose is synced, not animation state**: `skelAnime.jointTable[24]`
  (Vec3s rotation per joint), `prevTransl`, `movementFlags`, `upperLimbRot`,
  plus `currentBoots/currentShield/currentTunic`, `modelGroup`, `stateFlags1/2`,
  `itemAction`, `heldItemAction`, `buttonItem0`, `invincibilityTimer`,
  `actionVar1` (`Anchor.h` `AnchorClient`).
- **The receiver has no state machine.** The dummy player is a real
  `ACTOR_PLAYER` whose update/draw are replaced; it applies the client's
  `posRot` + a **joint-table pointer swap** (`player->skelAnime.jointTable =
  client.jointTable;`) + direct field copies (`DummyPlayer.cpp`). It keeps
  collision (cylinder, damage table), respects `stateFlags` for collision
  registration, and skips `PLAYER_STATE2_DISABLE_DRAW` on the wire.
- **Rationale**: syncing action/anim *state* would require the remote machine to
  run the same state machine; syncing the *resulting pose* makes the dummy
  exactly mirror the source player with zero coupling. **No interpolation** —
  the latest packet is applied directly.

The rest of this document re-derives the same strategy for TP, where the pose
lives in a different structure (J3D matrices, not a Vec3s joint table).

---

## 1. Where the TP Link pose lives (and how it is produced)

### 1.1 Pipeline

TP Link animation is J3D-based, not a hand-rolled skeleton walker:

```
per-frame (daAlink_c::execute, d_a_alink.cpp:18192)
  allAnimePlay()                       advance frame ctrls, play face textures
  setMatrix()            (:5877)       build base TR from current.pos + shape_angle
                                       (+ PROC-specific offsets: horse bob, boar,
                                        magnet boots …) → mpLinkModel->setBaseTRMtx
  modelCalc(mpLinkModel) (:19041 h, :19092 w)  → J3DModel::calc() →
       mDoExt_MtxCalcAnmBlendTbl(Old)::calc() (m_Do_ext.cpp:1111)
         per joint: sample up to 6 J3DAnmTransform packs
           (mNowAnmPackUnder[3] + mNowAnmPackUpper[3], d_a_alink.h:4040-4041)
           at the frame-ctrl frames, blend rotations via quaternion lerp,
           lerp translate/scale → local matrix
         concat with parent chain (mDoMtx_concat, m_Do_ext.cpp:35) →
         j3dSys.getModel()->setAnmMtx(jnt, mCurrentMtx)     ← THE POSE
  modelCallBack (d_a_alink.cpp:2433)   per-joint callback DURING calc:
                                       jointControll, foot/arm fixes at jnt 26,
                                       blend-rate morph at jnts 0/4/5/10/13/15
  setItemMatrix(0) / setWolfItemMatrix()  attach sword/shield/held-item models
                                       at mLeftItemJntNo/mRightItemJntNo
  setBodyPartPos() (:5687)             hands/feet/head/eye positions from pose
  setAttentionPos()
```

**The rendered pose is `J3DMtxBuffer::mpAnmMtx`** — an array of `Mtx` (4×4 f32,
root-relative *world-within-model* matrices), one per joint
(`J3DMtxBuffer.h:29,0x0C`). Read via `mpLinkModel->getAnmMtx(jnt)`
(`J3DModel.h:115`); written via `mpLinkModel->setAnmMtx(jnt, mtx)`
(`J3DModel.h:109-112`). The world placement is separate:
`mpLinkModel->getBaseTRMtx()`/`setBaseTRMtx` (`J3DModel.h:93-94`). The matrix
calc is per-joint installed **on the shared `J3DModelData` joint nodes**
(human: joints 0/1/16, wolf: 0/3/15 — see the fork's `modelCalc` comment,
`d_a_alink.cpp:19873`); the per-instance state is the six anm packs + frame
ctrls. `mAnmMtx` itself is per-model (each `J3DModel` owns its `J3DMtxBuffer`).

Models involved per Link (all attach to the body pose):

| model | field (d_a_alink.h) | role | pose source |
|-------|--------------------|------|-------------|
| body | `mpLinkModel` `:3936` | the skeleton/mesh | `mAnmMtx` (what we sync) |
| face | `mpLinkFaceModel` `:3937` | eyes/jaw expressions | own calc from `mFaceBck`/`mFaceBtp`/`mFaceBtk` + frame ctrl; base TR = body `anmMtx(4)` (`:6101`) |
| hat | `mpLinkHatModel` `:3938` | cap + wind tilt | own calc; base TR = body `anmMtx(4)` (`:6103`) |
| sword/shield/held | `mSwordModel` `:3962` / `mShieldModel` `:3946` / `mHeldItemModel` `:3982` | equipment | base TR = body `anmMtx(mLeftItemJntNo/mRightItemJntNo)` (`:6028,:6064`) |
| Midna (wolf) | `mpWlMidnaModel` etc. | wolf rider | base TR = body `anmMtx(0x19)` |

### 1.2 Joint counts

No compile-time constant in code; the count comes from the BMD at runtime:
`mpLinkModel->getModelData()->getJointNum()` (`J3DModelData.h:38`). Evidence
from in-tree code:

- Human skeleton: highest hard-coded indices are 13 (mouth,
  `getWolfMouthMatrix` is body), 26 (torso, foot/arm fix in `modelCallBack`),
  dynamic head via `field_0x30b4` — ~40 joints.
- Wolf skeleton: `setBodyPartPos` uses joints 0x13 (19, L hand), 0x18 (24,
  R hand), 0x1F (31, L foot), 0x24 (36, R foot), eye at jnt 4
  (`d_a_alink.cpp:5687`) → **≥ 37 joints**.

Packet carries a `u8 jointCount` so the exact number (40 human / 37+ wolf,
verified at runtime) is self-describing.

### 1.3 Pointer swap vs per-joint copy

Anchor swaps the jointTable **pointer** because OoT's `SkelAnime` owns a
`Vec3s*` the walker iterates. TP has **no equivalent**:

- The "pose" is not a list of rotations; it is a buffer of computed 4×4
  matrices owned by each model's `J3DMtxBuffer`. There is no indirection to
  re-point.
- The raw per-joint rotations exist only *inside* `J3DAnmTransform` (per anm
  pack, pre-blend, pre-callback) — reading those gives the *input* of one anim,
  not the blended/processed pose.

**→ per-joint copy is the only correct mechanism.** Read the sender's
`mAnmMtx` (40 × 64 B memcpy), write the puppet's `mAnmMtx` (40 × 64 B memcpy
via `setAnmMtx`). This is exactly as faithful as the matrices are, and the
per-joint callback / blend artifacts are already baked in on the sender.

A wrinkle: the **mtx calc pointer lives on the shared model data** (see the
fork's `modelCalc` comment, `d_a_alink.cpp:19873` — the fork re-arms it per
instance because `changeModelDataDirect` overwrites it). A stock binary with
multiple `daAlink_c` instances will compute *some* valid pose for each (the
last-armed calc), and **we don't care which** — the puppet's own calc output is
overwritten with received matrices in the same post-execute hook. This makes
the shared-model-data hazard structurally irrelevant to pose sync (it *does*
matter for the item/face model calcs, see §4.2).

---

## 2. Proposed `PlayerState` (pose variant)

Wire layout (little-endian, fixed structs per `00-network.md` §5). v1 sends
**raw matrices** (zero error, no math); the quantized variant is documented in
§6 as the only size optimization worth doing.

```
PlayerState (per remote player, EVERY frame @ game fps, unreliable-sequenced)
┌───────────────────────┬────────┬──────────────────────────────────────────────┐
│ field                 │ size   │ source (public reads on the local Link)      │
├───────────────────────┼────────┼──────────────────────────────────────────────┤
│ seq                   │ u16    │ mod-local monotonic counter                   │
│ playerId              │ u8     │ session id (host-assigned)                    │
│ pos                   │ 3×f32  │ fopAc_ac_c::current.pos  (f_op_actor.h:297)   │
│ yaw                   │ s16    │ fopAc_ac_c::shape_angle.y (f_op_actor.h:298)  │
│ pitch                 │ s16    │ daPy_py_c::mBodyAngle.x  (d_a_player.h:340)   │
│ baseTR                │ 64     │ mpLinkModel->getBaseTRMtx() (J3DModel.h:93)   │
│ form                  │ u8     │ daPy_py_c::checkWolf()  (d_a_player.h:1077)   │
│ jointCount            │ u8     │ mpLinkModel->getModelData()->getJointNum()    │
│ joint[0..39]          │ 40×64  │ mpLinkModel->getAnmMtx(j)  (J3DModel.h:115)   │
│   Mtx (root-relative) │        │                                              │
│ scaleFlags            │ 5      │ mpLinkModel->getMtxBuffer()->getScaleFlag(j)  │
│ faceBckIdx            │ u16    │ mFaceBckHeap.getIdx()  (d_a_alink.h:4055)     │
│ faceBtpIdx            │ u16    │ mFaceBtpHeap.getIdx()  (:4056)                │
│ faceFrame             │ s16    │ *field_0x215c->getFrame()  (:4058)            │
│ stateFlags            │ u8     │ see §2.1                                      │
│ roomNo                │ s8     │ current.roomNo  (f_op_actor.h:248)            │
├───────────────────────┼────────┼──────────────────────────────────────────────┤
│ TOTAL                 │ ~2.67KB│ ≈ 160 KB/s per player @ 60 Hz (see §6)        │
└───────────────────────┴────────┴──────────────────────────────────────────────┘
```

### 2.1 `stateFlags` byte (semantic state not carried by the pose)

| bit | meaning | source |
|-----|---------|--------|
| 0 | riding (any RIDETYPE) | `mRideStatus != 0` (`d_a_alink.h:4157`, enum `:1405`) |
| 1 | invulnerable / damage-blink | `mDamageTimer > 0` (`d_a_player.h:327`) |
| 2 | subjectivity (first-person) | `mProcID == PROC_SUBJECTIVITY` (`d_a_alink.h:1336`) |
| 3 | downed/dead (local life state) | coop-local `PlayerLifeState` (§5) |
| 4 | demo in progress | `mDemo.getDemoType() != 0` (`d_a_player.h:280`) |
| 5 | player no-draw (**never set on the wire**) | `checkPlayerNoDraw()` (`d_a_alink_link.inc:436`) — Anchor skips `DISABLE_DRAW` the same way |

`mProcID` (u16, `d_a_alink.h:4211`) is **not** sent — the pose replaces it for
rendering; the flags above are the only behaviors the puppet must know.

### 2.2 Reliable `PlayerEvent` (on change)

| field | size | source |
|-------|------|--------|
| equipItem | u16 | `mEquipItem` (`d_a_alink.h:4205`) |
| selectItemId | u8 | `mSelectItemId` (`:4143`) |
| sword/shield/clothes | 3×u8 | `dComIfGs_getSelectEquip{Sword,Shield,Clothes}()` (`d_com_inf_game.h:1398-1407`) — model selection |
| item joints | 2×u16 | `mLeftItemJntNo`/`mRightItemJntNo` (`d_a_alink.h:4317-4318`) |
| heldObject procName | u16 | `mItemAcKeep.getActor()` → `fopAcM_GetName` |
| ride status + horse entity | see §7 | `mRideStatus` + horse transform/proc/anim inline |
| attention/lock target | u16 | `mTargetedActor` (`:4072`) mapped to session entity id (**not** raw fpc id, §8) |
| downed → alive | u8 | local life state |

---

## 3. Read strategy (sender, stock binary)

Everything below is a direct public read in a **post-hook on
`daAlink_c::execute()`** (`d_a_alink.h:1811`, `DEFINE_HOOK(&daAlink_c::execute,
...)`). By the time `execute` returns, `setMatrix` → `modelCalc(mpLinkModel)` →
`setItemMatrix` → `setBodyPartPos` have all run, so `mAnmMtx` + `baseTRMtx` are
final for the frame.

| piece | read |
|-------|------|
| pose | loop `j = 0..jointCount-1`: `memcpy(dst+j, mpLinkModel->getAnmMtx(j), sizeof(Mtx))`; `scaleFlags` via `getMtxBuffer()->getScaleFlag(j)` (`J3DMtxBuffer.h:34`) |
| root | `mpLinkModel->getBaseTRMtx()`; `current.pos`; `shape_angle.y`; `mBodyAngle.x` |
| form | `checkWolf()` |
| face | `mFaceBckHeap.getIdx()`, `mFaceBtpHeap.getIdx()`, `field_0x215c->getFrame()` (face frame ctrl, public) |
| flags | `mRideStatus`, `mDamageTimer`, `mProcID`, `mDemo.getDemoType()` |
| appearance | `mEquipItem`, `mSelectItemId`, save getters §2.2 — on change only |

Send gating (mirror Anchor): skip the frame when no remote player is in the
same `roomNo`/stage; never transmit `checkPlayerNoDraw()` as "hidden".

---

## 4. Write strategy (puppet, stock binary)

The puppet is a **real `daAlink_c` created normally** (vanilla create, correct
form) whose input is zeroed. No state machine, no proc execution, no anim
replay. Two hooks on the puppet:

**Pre-hook** (before vanilla `execute`):
- set `current.pos`, `shape_angle.y`, `mBodyAngle.x` from the packet, and
  `speed`/`speedF` = 0 (physics still runs; output is overridden after).
- on form change (reliable event): `changeWolf()` / `changeLink(1)`
  (`d_a_alink.h:2889-2890`) *before* any pose — the skeleton differs.
- on appearance change: `setSelectEquipItem()` (`d_a_alink.cpp:4357`) +
  `mEquipItem`/`mSelectItemId`/item joints; face: `setFaceBck(idx)` +
  `setFaceBtp(idx)` + write `field_0x215c->setFrame(frame)`.

**Post-hook** (after vanilla `execute`; the vanilla calc output is discarded):
- `mpLinkModel->setBaseTRMtx(recv.baseTR)` — reproduces every `setMatrix`
  special case (horse bob, boar, magnet boots) with zero proc analysis.
- loop `j`: `mpLinkModel->setAnmMtx(j, recv.joint[j])` + `setScaleFlag(j, …)`
  (`J3DModel.h:109-112`, `J3DMtxBuffer.h:33`).
- re-derive presentation from the new pose: `setItemMatrix(0)` /
  `setWolfItemMatrix()`, `setBodyPartPos()`, `setAttentionPos()` — all public
  (they read `mAnmMtx`, so they pick up the received pose; they ran during
  `execute` with the stale pose, re-running is 1-frame-consistent).

The vanilla `execute` still runs fully (zeroed input): `Acch`/gravity keep the
puppet grounded, colliders stay live for local hit detection (that feeds the
combat channel), and the death/game-over path remains functional — Anchor's
dummy does the same (keeps cylinder + damage table, gates collision on
stateFlags).

**Why the shared-model-data mtx-calc hazard is not a problem**: the fork had to
re-arm per-instance mtx calcs (`d_a_alink.cpp:19873`) because
`changeModelDataDirect` overwrites the shared joint-node pointer. With pose
overwrite, whichever calc runs, its output is replaced. The only thing that
must still be correct per-instance is the **item/face model calcs** (they use
the body pose we just wrote) and the **form swap**.

---

## 5. Health / life — local only (unchanged from v1 of this report)

`daAlink_c` has no HP field; life is save-only (`dSv_player_status_a_c::mLife`,
`d_save.h:166-167`, read via `dComIfGs_getLife()`). Each machine owns its save
(`network.md` §2) → HP never crosses the wire. Remotes only need the
downed/dead *behavior* bit (`stateFlags.3`, derived from the coop-local
`PlayerLifeState`; the host's damage path is `setDamagePoint` →
`dusk::coop::combat::onPlayerDamaged`, `d_a_alink_damage.inc:220`). The
00-network provisional `hp u16` stays dropped.

---

## 6. Cadence and payload

**Cadence: every frame at game fps (60 Hz), no interpolation.** Anchor sends
every frame over the internet as JSON and works; on LAN with a compact binary
format, bandwidth is a non-issue. The hook runs every `execute`, gated by
same-room presence (§3). Receivers apply the latest packet directly
(Anchor-style); a stale/frozen remote player is detected by seq gaps + a
timeout (puppet freezes in last pose).

| encoding | per-frame | per player @ 60 Hz | 8 players |
|----------|-----------|--------------------|-----------|
| v1 raw `Mtx` per joint (§2) | ~2.67 KB | ~160 KB/s | ~1.3 MB/s |
| v1.5 quat+trans (below) | ~0.73 KB | ~44 KB/s | ~350 KB/s |

All within a LAN budget; even v1 raw is 8× smaller than Anchor's JSON per
frame. **Start with v1 raw** (simplest correct code: two memcpy loops), move to
v1.5 only if a constraint appears.

v1.5 quantization (documented, not built yet): per joint, decompose the
root-relative matrix into quat (4×s16, 8 B) + translation (3×s16, 6 B) +
scaleFlag bit — 40 × 15 B ≈ 600 B; root = pos + yaw + pitch + `procRootKind`
u8 (none/horse/boar/chain/magnet — so the puppet can reproduce `setMatrix`
offsets if baseTR is not sent). Not needed for LAN v1.

Delta policy: none needed at these sizes. `PlayerState` is fully self-contained
per frame (like Anchor), so packet loss costs one pose, never a desync. An
optional "full vs delta" split can be added later if the 60 Hz raw variant ever
matters; it is explicitly not required.

---

## 7. Horse riding (unchanged from v1, shrunk)

The Link riding pose is already in the synced joint matrices (the 
`ANM_HORSE_*`/`PROC_HORSE_*` pose IS the pose). What must still travel:

- `stateFlags.riding` + `mRideStatus` kind (horse/boar/canoe/board/spinner,
  `d_a_alink.h:1405-1410`).
- The **horse itself** — a separate `daHorse_c` actor with its own transform
  and anims. v1: inline the owned horse's `current.pos` (12 B), `shape_angle.y`
  (2 B), `m_procID` (u8, `d_a_horse.h:386`), `m_anmIdx[0..2]` (3×u16 `:399`,
  getter `getAnmIdx(i)` `:271`), frames `getAnmFrame(i)` (`:267`) into
  `PlayerEvent`/the player packet when riding; the host puppet places its own
  `daHorse_c` there (render-only; gameplay stays host-sim). Alternative (b):
  a horse entity channel — preferred once players ride each other's horses.
- `mRideActorID` (`d_a_alink.h:4097`) is a **local fpc id** — never on the
  wire (§8).

---

## 8. What is impractical (or wrong) to read from a mod

1. **Actor pointers / fpc process ids.** `mRideActorID` (`d_a_alink.h:4097`),
   `mTargetedActor` (`:4072`), `mAtnActorID`, `mItemAcKeep`/`mGrabItemAcKeep`
   inner ids — machine-local `fpc_ProcID`s; replace with session-scoped entity
   ids (`00-network.md` §11.3) or inline the state (horse §7).
2. **The anm packs / frame ctrls / blend ratios** (`mNowAnmPackUnder/Upper`,
   `mUnderFrameCtrl`, `mUpperFrameCtrl`, `mDoExt_AnmRatioPack::getRatio`) — the
   *inputs* of the pose. Sending them (v1 of this report) required the receiver
   to re-run the same blend/calc/callback chain and drifted; **dropped** in
   favor of the output (`mAnmMtx`). They remain useful only for the face
   (§4: `mFaceBckHeap.getIdx()` + frame ctrl) and for debugging.
3. **The per-joint callback's internal state** (`modelCallBack`,
   `jointControll`, `mFootData1/2`, `field_0x302c/0x3040` hat arrays) — all
   baked into `mAnmMtx` by the sender; nothing to sync.
4. **`mDoExt_MtxCalcOldFrame` internals** (`m_Do_ext.h:255`) — old-frame
   interpolation result is in the matrices; skip.
5. **`mCcStts` invulnerability internals** (`d_a_alink.h:4014`) — the visible
   blink is `mDamageTimer` (`d_a_player.h:327`), already in `stateFlags`.
6. **`mLeftItemJntNo`/`mRightItemJntNo` derivation** — the values are synced
   on change (§2.2); their per-anim selection logic (`setBodyPartFromAnm`)
   stays sender-side. Verify attach positions during puppet bring-up.
7. **Water/ground sensory state** (`mWaterY` `:4428`, `mLinkAcch` `:4024`,
   `mGndPolyAtt0` `:4174`) — host sims shared geometry; the puppet's own Acch
   computes these locally.
8. **A joint-table pointer swap like Anchor's** — structurally impossible in
   TP (pose is a per-model matrix buffer, not a re-pointable Vec3s array); the
   equivalent is the per-joint `setAnmMtx` copy (§1.3).

---

## 9. Open questions / risks

1. **Puppet identification and creation.** The mod creates the host's puppets
   itself and tags `daAlink_c* → session PlayerId` at `daAlink_c::create()`
   (`d_a_alink.h:1479`) post-hook; the client's own Link is its tagged local.
   Works in a stock binary — no fork registry.
2. **Vanilla execute on the puppet is wasted work** (physics, action checks
   with zeroed input, damage path). Cheap enough; but confirm zeroed-input
   `mItemTrigger`/`mItemButton` cannot trip a proc (e.g. a forced item use).
   Fallback: skip the original `execute` in a replace-hook and only run
   `setMatrix`/`setBodyPartPos` ourselves — all public.
3. **1-frame staleness of derived positions.** `setItemMatrix`/`setBodyPartPos`
   run inside `execute` before our post-hook pose write; the post-hook re-runs
   them, so the *draw* is consistent. Verify the draw never reads an
   intermediate (it uses `mAnmMtx` + `baseTRMtx` — both ours by then).
4. **Form swap ordering.** Human/wolf arcs and skeletons differ; a pose from the
   wrong form is garbage. Apply `PlayerEvent(form)` before subsequent poses
   (§4 pre-hook); keep form atomic with the first pose after the change.
5. **Joint count differences (human ~40 vs wolf ≥37).** `jointCount` in the
   packet makes it self-describing; the puppet must already be in the matching
   form (skeleton loaded) before matrices are written.
6. **Face expressions.** Synced as {bckIdx, btpIdx, frame}; the face model's
   own calc runs in vanilla `execute` (via `playFaceTextureAnime`) with those
   inputs, so expressions reproduce. Verify `field_0x215c` is the face frame
   ctrl for both forms, and that BTP (texture) swaps land.
7. **pvp/damage gating on the puppet** (Anchor keeps a damage table and gates
   collision on stateFlags; `invincibilityTimer` in Anchor is our
   `mDamageTimer`/`stateFlags.1`). Collision/combat details belong to the
   combat investigation; the pose surface feeds it `stateFlags` only.
8. **Frozen/stale puppet policy.** seq-gap timeout → freeze last pose + fade;
   no interpolation per Anchor. Revisit only if internet play appears
   (`network.md` §12 non-goal).
