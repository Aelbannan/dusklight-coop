# Puppet Link — spawning and driving a second `daAlink_c` from a pure `.dusk` mod

Status: **Design investigation** (no code changed). Target: **STOCK TwilitRealm/dusklight**
(upstream `main`), reachable only through the mod SDK — hooks by symbol, direct calls into
game functions, data-field reads/writes. No `TARGET_PC` edits, no fork symbols.

File/line references are to **stock upstream** unless marked `(fork)` — the in-tree coop fork
(used as a reference throughout) is a few hundred lines ahead, so fork line numbers are given
where the fork version is being described. All functions named below are symbol-manifest-visible
(public, private, or file-local static) and therefore hookable via `DEFINE_HOOK` /
`DEFINE_HOOK_SYMBOL`, or directly callable because a mod links against the game binary
(`docs/modding.md` "Hooking Game Functions", `sdk/CMakeLists.txt` link stubs).

Section 5 evaluates Ship of Harkinian's "Anchor" co-op pattern (OoT/MM) against this engine —
it solves the same problem with a proven spawn-and-hijack approach, and every one of its
mechanisms has a TP analogue, several of which are *simpler* than in OoT.

---

## 0. Executive summary

| Question | Verdict |
|---|---|
| Can a mod create a second `daAlink_c`? | **Yes — pure mod.** `fopAcM_create(fpcNm_ALINK_e, …)` is a public exported function; the process framework does **not** deduplicate by proc name. The in-tree fork already does exactly this at runtime (`src/dusk/coop/coop_player.cpp:353`). |
| Does a second Link break the engine? | **Only three things need neutralizing at create time** (global player/Link pointer clobber, Midna/NPC/horse spawn fan-out, restart-room save write) and **one at draw time** (shared-J3DModelData mtxCalc overwrite). All four are mod-hookable. Everything else (camera, enemy AI, demos, HUD, sound) keeps reading slot-0 = the real player, which is *correct* for the networked model. |
| Can a mod freeze the puppet's own simulation? | **Yes, two ways.** (A) Replace-hook `daAlink_c::execute` and return early for the puppet. (B, Anchor-style) Write `actor->sub_method` to a custom `actor_method_class` table so the vanilla Execute/Draw/Delete are never dispatched for the puppet at all. |
| Can a mod drive the puppet from received state? | **Yes.** TP is matrix-driven: copy `current.pos`/`shape_angle`, `mProcID`, form flag, and the pose either via anm-id + frame controls, or by copying a received joint-matrix table straight into the model via `J3DModel::setAnmMtx`. |
| What is impossible from a pure mod? | Only the **per-actor "current player" context swap** (the fork's `ScopedContext` + header-inline edits) and the **multi-viewport renderer**. Neither is needed: a networked puppet is passive and each machine renders one view of its own Link. No `numLoaded`-style actor-count machinery exists in TP to fight. |

The one structural fact that shapes everything: in stock, `dComIfG_play_c` is sized for **one**
player (`mPlayerInfo[1]`, `mPlayerPtr[2]`, `mPlayerStatus[1][4]`, `mWindow[1]`,
`mCameraInfo[1]` — `include/d/d_com_inf_game.h:944-952` stock). The fork **grew those arrays**
(`MAX_PLAYERS`/`MAX_VIEWS`) — an ABI change a mod cannot make. A mod therefore keeps the
puppet in **its own registry** and must prevent the puppet from ever being written into the
native slot-0 pointers.

---

## 1. How vanilla creates the player's Link

Creation path (all stock):

1. **Stage script table** — `dStage_dt_c` dispatch table maps `"PLYR"` → `dStage_playerInit`
   (`src/d/d_stage.cpp:1643`, table at 2658).
2. **`dStage_playerInit`** (`d_stage.cpp:1643`) — guarded by
   `if (dComIfGp_getPlayer(0) != NULL || …) return 1;` (line 1650): only one player Link per
   stage. Builds a `fopAcM_prm_class` from the start-point data and calls
   `dStage_actorCreate(player_data, appen)` (line 1716).
3. **`dStage_actorCreate`** (`d_stage.cpp:1595`) — resolves `"Link"` →
   `fpcNm_ALINK_e` via `dStage_searchName` (line 1526; `OBJNAME("Link", fpcNm_ALINK_e, -1)`
   at line 535), then `fopAcM_Create(fpcNm_ALINK_e, NULL, i_actorPrm)` (line 1608).
4. **`fopAcM_Create`** is an inline over `fpcM_Create` → `fpcSCtRq_Request(fpcLy_CurrentLayer(), …)`
   (`include/f_pc/f_pc_manager.h:45`) — creates a process on the **current layer**, no proc-name
   dedup. The profile's Layer ID is `fpcLy_CURRENT_e`, so the layer at request time sticks.
5. **Profile** — `g_profile_ALINK` (`d_a_alink.cpp:19927`), methods
   `daAlink_Create` (static, `d_a_alink.cpp:5124`) → `daAlink_c::create()` (member,
   `d_a_alink.cpp:4887`), `daAlink_Execute` (static, 18924) → `execute()` (17798),
   `daAlink_Draw` (static, 19862) → `draw()` (19466). Draw priority `fpcDwPi_ALINK_e`,
   status `fopAcStts_UNK_0x40000 | NOPAUSE | FREEZE`.
6. **`daAlink_c::create()`** is multi-phase (returns `cPhs_INIT_e` until complete):
   - Phase 1 (`!createWaitFlg`): **writes the global pointers**
     `dComIfGp_setPlayer(0, this); dComIfGp_setLinkPlayer(this);` (stock 4924-4925) — see §2,
     the main thing a puppet create must not be allowed to do.
   - Resource loads: Alink arc on its own heap (`dComIfG_resLoad(&mPhaseReq, mArcName, …)`),
   shield arc, solid heap `0x3E930` (`fopAcM_entrySolidHeap(this, daAlink_createHeap, …)`).
   - `playerInit()` (stock 4454): collision bodies, anm heaps, `mZ2Link.init(…)`, form
     `changeLink(0)`/`changeWolf()`. Reads save for inventory/equipment (reads only).
   - `mLinkAcch.CrrPos(dComIfG_Bgsp())` ground sanity; `setRoomInfo()`, `setWaterY()`.
   - **`dComIfGs_setRestartRoom(current.pos, shape_angle.y, getStartRoomNo())`** (stock 5031)
     — writes the *host's* save restart point. Must be suppressed for puppets.
   - `int midna_prm = setStartProcInit();` (stock 5061; `setStartProcInit` at 4689) — story
     init; already falls back to `procWaitInit()` in several branches (4778).
   - Tail (stock 5104-5105): **`fopAcM_create(fpcNm_MIDNA_e, midna_prm, …)`** and
     `checkSetNpcTks(…)` — spawns Midna + NPCs. Must be suppressed for puppets.
   - `fopAcM_RegisterCreateID` is a debug-logging macro only (`f_op_actor_mng.h:41`) — no
     dedup, no constraint.
7. **Destructor path** — `daAlink_Delete` → `~daAlink_c()`; clears player status flags for slot 0
   (`dComIfGp_clearPlayerStatus0(0, …)`). On stage change, `dComIfG_play_c::init()`
   (`src/d/d_com_inf_game.cpp:44`) zeroes `mPlayerInfo`, `mCameraInfo`, `mPlayerStatus`,
   `mPlayerPtr` wholesale.

**Create is async, not synchronous — use `fopAcM_create`, never `fopAcM_fastCreate`.** The
standard create request (`fpcSCtRq_Request`) is a multi-frame phase state machine
(`fpcSCtRq_phase_Load → CreateProcess → SubCreateProcess → IsComplete → PostMethod`,
`src/f_pc/f_pc_stdcreate_req.cpp`), re-invoked until `daAlink_c::create` returns
`cPhs_COMPLEATE_e`. `fpcM_FastCreate` (`f_pc_manager.cpp:135`) runs `fpcBs_SubCreate`
synchronously and returns the actor only if it completes in one call
(`f_pc_fstcreate_req.cpp:47`) — impossible for ALINK (multi-frame resource loads), so it would
return NULL and cancel the request. The fork uses the async `fopAcM_create` + poll pattern
(`coop_player.cpp:353-376`).

**Verdict: creation is fully reachable from a mod.** The framework imposes no single-Link
constraint; the only obstacles are the side effects in step 6 (global pointer clobber, save
write, Midna/NPC spawns), all neutralizable with hooks (§4/§6).

---

## 2. Global state that assumes exactly one Link

Stock layout: `dComIfG_play_c` (`include/d/d_com_inf_game.h` stock 944-952):
`mPlayerInfo[1]`, `mPlayerPtr[2]` (0=Link, 1=Horse), `mPlayerStatus[1][4]`, `mWindow[1]`,
`mCameraInfo[1]`. Accessors are **header inlines** (not symbols), so a mod cannot hook them;
they compile into every caller. Per-consumer analysis:

### 2.1 `dComIfGp_getLinkPlayer()` / `dComIfGp_getPlayer(0)` (the global Link)
- Stock: `dComIfGp_getLinkPlayer()` → `play.getPlayerPtr(LINK_PTR)` (d_com_inf_game.h:3570);
  `daAlink_getAlinkActorClass()` (`d_a_alink.h:8458`) and `daPy_getLinkPlayerActorClass()`
  (`d_a_player.h:1315`) wrap it. `dComIfGp_getPlayer(0)` → `mPlayerInfo[0].mpPlayer`.
- **What breaks:** a second Link's `create()` **overwrites both** (stock 4924-4925). After that,
  every consumer (camera, enemies, events, HUD, `dStage_playerInit` guard) would see the
  puppet as "the Link".
- **Verdict: mod-hookable.** Replace-hook `daAlink_c::create`; for the puppet, save
  `mPlayerInfo[0].mpPlayer` + `mPlayerPtr[0]` before `g_orig(...)` and restore them after
  every phase call. The clobber window is inside the create call only; camera/enemies read
  these pointers later in the frame. A mod can read/write these fields because it links the
  binary and includes the stock headers.
- **No list-scan lookup to worry about:** TP finds "the player" by *stored pointer*, not by
  scanning a player category. There are **zero** `fopAcM_SearchByName(fpcNm_ALINK_e, …)` call
  sites in `src/` — nothing enumerates ALINK actors by procname, so a second ALINK never
  becomes "the player" by accident. (Contrast OoT's `GET_PLAYER` = head of the
  `ACTORCAT_PLAYER` list — see §5.)
- **Deliberately do NOT** try to make the puppet "the Link" for gameplay: all consumers read
  slot 0, and in the networked model they should keep reading the real player.

### 2.2 Camera (`src/d/d_camera.cpp`)
- The chase target is **per-camera, not the global Link**: `get_player_actor(cam)` =
  `dComIfGp_getPlayer(dComIfGp_getCameraPlayer1ID(cam_id))` (d_camera.cpp:225-227), consumed
  in `dCamera_c::Run()` (stock 1053) which stores it in `mpPlayerActor` (line ~1074) and uses
  `linkActor()` (339) throughout.
- **What breaks:** nothing, as long as the puppet never occupies `mPlayerInfo[0]` (it won't,
  after the §2.1 restore). The camera chases the real Link, which is what each machine wants.
  `dCam_getControledAngleY(dComIfGp_getCamera(field_0x317c))` in `setStickData`
  (d_a_alink.cpp:9386) — reads camera 0 by default; a frozen puppet never runs it.
- **Verdict: safe, no mod work needed** (single-machine single-camera model).

### 2.3 Enemy AI targeting
- Enemies find their target through the **inlines** `daPy_getPlayerActorClass()` /
  `daPy_getLinkPlayerActorClass()` → slot 0. `rg` shows 100+ actor files referencing
  `daPy_getLinkPlayerActorClass` (e.g. `d_a_e_mf`, `d_a_obj_cho`).
- **What breaks:** nothing for a *passive* puppet — enemies aggro the local (host) Link only.
  Under the networked model, host enemies are host-authoritative, so this is *correct*.
- **Verdict: safe for v1; hostile-enemy-aggro-on-puppet is NOT mod-feasible** in the general
  case: every enemy file calls an inline accessor, and retargeting means replacing each
  enemy's target resolution. If combat against remote players is wanted later, do it at the
  collision layer — which is exactly what Anchor does (puppet keeps its colliders; a hit is
  detected by the collision system and resolved as a damage *intent* on the wire, not by
  editing enemy AI). See §5.

### 2.4 Demo / event system
- `dEvent_c` entry, `d_ev_camera`, `d_event_data.cpp:891` read `dComIfGp_getLinkPlayer()` /
  `daAlink_getAlinkActorClass()` (e.g. `d_event.cpp:1301`) — always slot 0.
- **What breaks:** demos star the *real* Link; a frozen puppet is inert (its update never
  runs, so demo code inside Link can't act on it). Risk is only if a mod *swaps* the Link
  pointer (v1: never swap). The fork's per-player demo arbitration
  (`coop_event.h` `WaitingForParty`, `deferStageEntry`, …) is split-screen machinery and is
  **deliberately removed** in the networked design (`docs/design/network.md` §8).
- **Verdict: safe for v1. Minor cosmetic: puppets stand frozen during the local player's
  cutscenes.**

### 2.5 Sound
- Per-instance `mZ2Link` (`playerInit`, d_a_alink.cpp ~4492) — each Link has its own sound
  source; a frozen puppet plays nothing (no footstep/voice calls from a skipped update).
  `mZ2Link.init(&current.pos, …)` positions it, so a moved puppet's voice source (if any were
  triggered) would track. Global sound manager keeps the camera/player listener on the real
  Link.
- **Verdict: safe; verify `Z2GetAudioMgr` listener position during playtest.**

### 2.6 Attention / Z-targeting
- One global `dAttention_c` (`dComIfGp_getAttention()`, d_com_inf_game.h:3117); the puppet's
  `mAttention` is assigned to it. The real Link does all Z-targeting. A frozen puppet never
  registers as a target source.
- **Verdict: safe.**

### 2.7 HUD / life / game over
- `dMeter2Info`, `dComIfGs_getLife` etc. are global/save — tied to the one real Link.
  **Danger:** the puppet's own damage path would drain the *host's* hearts
  (`dComIfGp_setItemLifeCount(-dmg, 0)` inside `daAlink_c::setDamagePoint`, damage.inc:177)
  and, if HP hit zero, the death/game-over flow lives inside `execute()` — both neutralized by
  (a) skipping the puppet's update (no death procs) and (b) hooking `setDamagePoint` (skip for
  puppets).
- **Verdict: mod-hookable, required.** See §6.3.

### 2.8 Room / stage transitions
- Room change: room-layer actors are swept; the Link (and a puppet created on the **play-scene
  layer**, §1.4) survives because it lives on the stage layer. `dStage_RoomCheck` is per-Link
  via `setRoomInfo()`.
- Stage change: everything dies. The fork deletes + re-creates non-authority Links in
  `dScnPly_Delete` (`d_s_play.cpp:862` fork / stock 836, file-local static) and
  `dScnRoom_Delete` (`d_s_room.cpp:338` fork / stock 325); a mod can hook the same two
  statics, or simply poll in `mod_update` and re-spawn when the puppet's pid is gone and a new
  stage's Link exists.
- **Verdict: mod-hookable.** Spawn layer trick is required (§6.1).

### 2.9 No `numLoaded` / actor-count machinery
- OoT's `Actor` struct lives in a global actor DB keyed by actor id, with per-id `numLoaded`
  counters that trip debug asserts if a spawned actor never "registers" as its real id
  (Anchor swaps `actor->id` to a ghost id and back in destroy to keep the counter sane).
  **TP has no equivalent:** `f_pc`/`f_op` track neither per-procname instance counts nor
  loaded counts (searched: no `numLoaded`/`num_loaded` anywhere in `src/f_pc`, `src/f_op`,
  `include/f_pc`, `include/f_op`). The fork runs up to 8 ALINK processes simultaneously in
  debug builds with no such assert. The puppet keeps procname `fpcNm_ALINK_e` and needs no
  id-juggling on spawn or destroy.
- **Verdict: nothing to do.** (The only debug-build quirk: the HIO per-actor stop/step tool in
  `fopAc_Execute` matches by procname, so "stop ALINK" in the HIO stops all Links — debug-only
  tooling, not a crash.)

---

## 3. What the in-tree coop had to patch (reference for a mod)

The fork tolerates N Links via in-source `TARGET_PC` edits — this is the checklist of what the
engine needs. A mod must replicate each with a runtime hook:

| Fork edit | Where (fork) | What it does | Mod equivalent |
|---|---|---|---|
| Per-player Link storage + context-scoped `getLinkPlayer()` | `include/d/d_com_inf_game.h` (struct growth `MAX_PLAYERS`/`MAX_VIEWS`, `playerForContextBridge`) | Header-inline re-routing of every "the Link" read to a per-player slot | **Impossible** (ABI, inline) — **not needed**: puppet is passive; keep a mod-side registry |
| `ScopedContext` stack pushed in `daAlink_Execute`/`daAlink_Draw`/`fopAc_Execute` | `d_a_alink.cpp:19427` (fork), `f_op_actor.cpp:293` (fork, NPC context) | Makes inline `currentPlayer()` reads resolve per actor | **Impossible** (code injection into mid-function) — **not needed** for passive puppets |
| Puppet-safe create: `usesStoryStart` branch → `procWaitInit()`, skip Midna/NPC/restart-room, indexed spawn params | `d_a_alink.cpp:4922-5250` (fork) | Stops story side effects for non-authority Links | Replace-hook `daAlink_c::create` + spawn-suppression hooks (§6.2) |
| Indexed `fopAcM_create(fpcNm_ALINK_e, …)` on the play-scene layer | `src/dusk/coop/coop_player.cpp:338-361` | Spawns up to `MAX_LOCAL_PLAYERS` real `daAlink_c` | Same call + `fpcLy_SetCurrentLayer` from a mod — both public |
| Input snapshot feeding (`applyInputSnapshot`) in `setStickData` | `d_a_alink.cpp:9590-9603` (fork) | Drives each Link from its own pad snapshot | Not needed (networked puppets don't take input); alternatively replace-hook `setStickData` to zero sticks |
| Combat: friendly-fire filter, cut-type attribution, suppress game-over | `src/d/d_cc_s.cpp:543`, `d_cc_uty.cpp:99`, damage.inc:164-225 | `dCcS::Set` / `dCcS::CalcAtTg` intercepts, per-player damage routing | Mod: replace-hook `daAlink_c::setDamagePoint` + `dCcS::CalcAtTg` if FF is ever needed |
| Shared-J3DModelData mtxCalc restore in `modelCalc` | `d_a_alink.cpp:19875-19892` (fork) | Two Links sharing the same model data fight over joint mtxCalc pointers | Replace-hook `daAlink_c::modelCalc` (§6.3) — **required for correct puppet rendering** |
| Per-player cameras / windows / HUD | `coop_camera.cpp`, `coop_render.cpp`, `coop_hud.cpp` | Split-screen viewports | Not needed — networked model is one camera per machine |
| Room/stage recreate | `coop_player.cpp:490` `onRoomUnload`, `d_s_play.cpp:862`, `d_s_room.cpp:338` | Delete/re-create Links across transitions | Hook the same two statics or poll in `mod_update` |

Key takeaway: **the fork's hard parts (per-actor context, per-player storage, split-screen)
are exactly the parts a networked puppet does not need.** Everything a passive puppet needs is
on the "mod-equivalent" column and is hook/call territory.

---

## 4. Anchor pattern at a glance

For readers who haven't seen Ship of Harkinian's `soh/soh/Network/Anchor`: it spawns a real
`ACTOR_PLAYER` for each remote client, then — via a `ShouldActorInit` hook that fires between
spawn and first update — **hijacks the actor**: `actor->id = ACTOR_EN_OE2`, category moved to
`ACTORCAT_NPC`, and init/update/draw/destroy replaced with `DummyPlayer_*`. The custom init
runs the real `Player_Init` under a temporarily juggled `gSaveContext.linkAge`; the custom
update copies the remote pose in (`skelAnime.jointTable` rotation array, pos/rot, state flags,
item/model fields) and re-registers OC/AC/AT colliders each frame with a custom damage table;
the custom draw runs the real `Player_Draw` under juggled save fields; destroy resets
`actor->id = ACTOR_PLAYER` so the DB's `numLoaded` decrements correctly. A per-actor
side-table (`ObjectExtension`) maps actor → clientId.

Section 5 maps each of these mechanisms onto TP/Dusklight.

---

## 5. Reference: SoH Anchor pattern vs. TP/Dusklight

### 5.1 Spawn — what is the TP analog of `Actor_Spawn` for a Link?

`Actor_Spawn(gPlayState->actorCtx, gPlayState, ACTOR_PLAYER, pos, rot, params)` →
**`fopAcM_create(fpcNm_ALINK_e, 0xFFFF, params, &pos, roomNo, &angle, nullptr, -1, nullptr)`**
on the play-scene layer (`f_op_actor_mng.cpp:255`; public, exported; the fork's exact call at
`coop_player.cpp:353`, with the `fpcLy_SetCurrentLayer` dance at 338-350). Two differences vs
OoT matter:

- **TP's spawn is async** (multi-phase create, §1) — poll `fopAcM_SearchByID(pid)` until the
  ALINK actor exists, exactly like the fork (`coop_player.cpp:370-376`). `fopAcM_fastCreate`
  is not viable (§1).
- **The player is not found by category scan.** Anchor *must* move the dummy out of
  `ACTORCAT_PLAYER` because OoT's `GET_PLAYER` is the head of that list. TP's "the player" is
  the stored `mPlayerInfo[0]`/`mPlayerPtr[LINK_PTR]`; the puppet keeps procname
  `fpcNm_ALINK_e` and no category move is needed — the only leak is the create-time pointer
  clobber (§2.1), fixed in the create hook.

### 5.2 Hijack point — is there a `ShouldActorInit` equivalent?

**OoT:** `Actor_Spawn` allocates + enqueues the actor; init runs lazily on its first update
pass; SoH's `ShouldActorInit` fires in that window, before `Player_Init` executes.

**TP: there is no lazy init.** The f_op framework is strictly sequential: the create request
pipeline (`fpcSCtRq_Request` → `fpcSCtRq_phase_SubCreateProcess` →
`fpcBs_SubCreate` → `fpcMtd_Create` = `daAlink_c::create`) runs to `cPhs_COMPLEATE_e`, and
**only then** does `fpcCtRq_Do` call `fpcEx_ToExecuteQ` to put the process in the execute queue
(`src/f_pc/f_pc_create_req.cpp:115`; `fpcEx_ToLineQ` sets `init_state = 2`,
`f_pc_executor.cpp:55`; `fpcEx_Execute` runs only when `init_state == 2`,
`f_pc_executor.cpp:33`). A process can never update or draw before its create completes. The
"between spawn and first update" window is therefore **the create call itself** — and the mod
is already inside it (replace-hook on `daAlink_c::create`). Anchor hijacks before init; TP
hijacks *at* init. Both are before first update; TP's point is strictly stronger (create has
not yet polluted the globals when the mod runs).

Even better, TP gives a true per-instance function-pointer swap (Anchor's other mechanism):
every actor carries `sub_method` (`f_op_actor.h:18`,
`/* 0x24 */ DUSK_CONST actor_method_class* sub_method`), assigned from the profile at create
start (`f_op_actor.cpp:480`) and used by **all four** lifecycle dispatches:
- execute: `fopAc_Execute` → `fpcMtd_Execute(actor->sub_method, actor)` (`f_op_actor.cpp:371`)
- draw: `fopAc_Draw` → `fpcLf_DrawMethod((leafdraw_method_class*)actor->sub_method, actor)`
  (`f_op_actor.cpp:264`)
- is-delete: `f_op_actor.cpp:421`; delete: `f_op_actor.cpp:443`

`actor_method_class` is a 0x20-byte table (`f_op_actor.h:11`): `{ create(0x0), delete(0x4),
execute(0x8), is_delete(0xC), draw(0x10) }` (`f_pc_method.h:8`,
`f_pc_leaf.h:11`); `l_daAlink_Method` = `{ daAlink_Create, daAlink_Delete, daAlink_Execute,
NULL, daAlink_Draw }` (d_a_alink.cpp:19921). **A mod can write `actor->sub_method = &customTable`
(a mod-defined static `actor_method_class`) at the end of the puppet's create, before the
framework enqueues it for execute** — from that point on the vanilla `daAlink_Execute`,
`daAlink_Draw` and `daAlink_Delete` are never dispatched for that instance. This is the direct
TP translation of "actor->init/update/draw/destroy replaced with DummyPlayer_*".

(Alternative without touching `sub_method`: replace-hook the static `daAlink_Execute` /
`daAlink_Draw` and branch per instance on "is this the puppet". Both work; §6 recommends the
swap as the Anchor-faithful primary.)

### 5.3 Pose surface — does daAlink_c have an `skelAnime` analog?

**OoT** is rotation-driven: `SkelAnime.jointTable[24]` holds `Vec3s` per-bone *rotations*; the
draw routine computes matrices from them. Anchor's packet therefore carries 24×`Vec3s` + a
`prevTransl`/`movementFlags` translation hint, and the dummy either pointer-swaps
`jointTable` or copies it — no animation state machine runs on the dummy.

**TP is matrix-driven.** The pose lives in the model, not a rotation table:
- Root transform: `J3DModel::getBaseTRMtx()` / `setBaseTRMtx(Mtx)` (`libs/JSystem/.../J3DModel.h:93-94`),
  written by `daAlink_c::setMatrix()` (stock 5744) from `current.pos`/`shape_angle`.
- Per-joint matrices: `J3DMtxBuffer::mpAnmMtx[idx]`, exposed as
  `J3DModel::MtxP getAnmMtx(int jointNo)` (readable *and* writable pointer) and
  `J3DModel::setAnmMtx(int jointNo, Mtx)` (copies in) — `J3DModel.h:109-116`,
  `J3DMtxBuffer.h:29-30`. `mMtxBuffer` itself is a plain field (`J3DModel.h:127`).
- The joint matrices are computed each frame by `J3DModel::calc()` from the per-instance
  `mDoExt_MtxCalcAnmBlendTblOld` (`field_0x1f20` under / `field_0x1f24` upper, d_a_alink.h:4038-4041)
  over the `mNowAnmPackUnder/Upper` anm packs and `mUnderFrameCtrl`/`mUpperFrameCtrl`
  (`daPy_frameCtrl_c : J3DFrameCtrl`, d_a_alink.h:4044-4045), driven by
  `allAnimePlay()` (stock 7258).
- Draw consumes the same table: `daAlink_c::draw` and `setDrawHand` read
  `mpLinkModel->getAnmMtx(0x13/0x14/0x15/9/0xE/…)` for hands/face/etc.

So the TP analog of "copy the remote pose into `skelAnime`" has two viable shapes:

- **(i) Anm-driven (compact).** Packet carries (action, anm-id, frame, rate); the puppet's
  update calls `setSingleAnimeBase(...)` / `setUpperAnimeParam(...)` (or writes
  `mUnderFrameCtrl[i].setFrameCtrl(...)` directly) then `setMatrix` → `allAnimePlay` →
  `mpLinkModel->calc()`. ~bytes/frame; requires an anm-id mapping table and doesn't reproduce
  subtle blend/phase states exactly.
- **(ii) Matrix-copy (exact — Anchor's approach).** Packet carries the joint matrix table
  (~30 joints × `Mtx` 48B ≈ 1.4 KB at 30 Hz; LAN-trivial per `network.md` §4). The puppet's
  update writes `current.pos`/`shape_angle`, calls `setMatrix()` (root), then copies the
  received matrices per joint via `getAnmMtx(j)`/`setAnmMtx(j, mtx)` *after* `calc()` (or
  skips `calc()` for the joint-bearing models). Bit-for-bit remote pose, no animation mapping.

Anchor's `upperLimbRot`/`movementFlags` translation-apply step maps to TP's `current.pos` +
`mLinkAcch.CrrPos` ground handling in the puppet update (§6.4). Recommended: (i) for v1,
(ii) when exactness matters.

### 5.4 Global-save juggling — does TP need `gSaveContext` juggling?

**OoT** reads `gSaveContext` heavily in both `Player_Init` and `Player_Draw` (age, button
items, tunics); Anchor juggles `linkAge` (and `buttonItems[0]` in Draw) around the vanilla
calls.

**TP reads far less global save state post-create:**
- `checkWolf()` is **instance state** — `checkNoResetFlg1(FLG1_IS_WOLF)`
  (`d_a_player.h:1077`), set at create from `dComIfGs_Wolf_Change_Check()` (d_a_alink.cpp ~5005)
  and never re-read from save afterward. A mod flips the puppet's form with
  `onNoResetFlg1/offNoResetFlg1(FLG1_IS_WOLF)` + `changeLink(0)`/`changeWolf()`.
- `daAlink_c::draw` (stock 19466) reads **per-instance fields** (`mEquipItem`,
  `mLeftHandIndex`, `mRightHandIndex`, `checkWolf()`, `mProcID`) — the mod writes those
  directly from the packet; no save juggling at draw time.
- The only save reads that matter are at **create**: form (`dComIfGs_Wolf_Change_Check` —
  decides which arc/model loads), wear (`dComIfGs_getSelectEquipClothes`), `getLastSceneMode`,
  `getStartMode`, plus the write `dComIfGs_setRestartRoom`. A mod can either (a) accept the
  host's state and override afterward (v1), or (b) do exactly Anchor's juggle — save the
  `dSv_info_c` fields, force "human, default equip", call `g_orig`, restore — inside the
  create replace-hook. (b) is cheap: the fields are plain members of
  `g_dComIfG_gameInfo.save` (`dSv_info_c`), mod-writable.
- **Verdict: TP needs less juggling than OoT.** One create-time juggle (form/wear) at most;
  zero at update/draw.

### 5.5 Actor-count/debug asserts — is there a `numLoaded` problem?

OoT: `ActorDB` per-id `numLoaded` counters → Anchor swaps `actor->id` to `ACTOR_EN_OE2` on
spawn and back to `ACTOR_PLAYER` in `DummyPlayer_Destroy` so the counter decrements correctly.

TP: **no such machinery** (§2.9). No per-procname counts, no debug asserts on actor totals in
`f_pc`/`f_op`. The puppet's `~daAlink_c()` does call `dComIfGp_clearPlayerStatus0(0, …)`
(stock destructor) — clears the host's slot-0 status bits on despawn; harmless because the
frozen puppet never set meaningful statuses (do not fight it).

### 5.6 The remaining Anchor mechanisms and their TP counterparts

| Anchor (OoT/SoH) | TP/Dusklight equivalent | Verdict |
|---|---|---|
| Spawn `ACTOR_PLAYER`, then `ShouldActorInit` hijack before first update | No lazy init; the create pipeline runs to completion before the process enters the execute queue (`f_pc_create_req.cpp:115`, `f_pc_executor.cpp:33`). Hijack point = replace-hook on `daAlink_c::create` (during init) + optionally install custom `actor->sub_method` (f_op_actor.h:18; dispatched at f_op_actor.cpp:264/371/421/443) before first execute | **Mod-hookable; TP's point is before any global pollution** |
| `actor->id = ACTOR_EN_OE2`, category → `ACTORCAT_NPC` | Keep `fpcNm_ALINK_e`; player found by stored pointer, not category scan; zero `SearchByName(ALINK)` call sites; no `numLoaded` | **Not needed** |
| `spawningDummyPlayerForClientId` flag + side-table `ObjectExtension` (actor→clientId) | Mod-side flag set right before `fopAcM_create`; `std::map<fpc_ProcID, ClientId>` registry keyed by the returned pid (no spare field on `fopAc_ac_c`; the fork does exactly this in `coop_alink.cpp` `g_spawns`/`g_ownedLinks`) | **Mod-side, trivial** |
| Custom init runs real `Player_Init` with juggled `gSaveContext.linkAge` | Custom create calls real `daAlink_c::create` via `g_orig` with (a) pointer save/restore, (b) Midna/NPC/horse spawn suppression, (c) `dComIfGs_setRestartRoom` suppression, (d) optional `dSv_info_c` form/wear juggle | **Mod-hookable** (§6.2) |
| Update copies `jointTable` rotations, pos/rot, state flags, item/model fields | Matrix-driven: `current.pos`/`shape_angle` + `mProcID` + form flag + `mEquipItem`/hand indices + pose via anm-id/frames **or** direct `setAnmMtx` matrix copy; reposition colliders with `setCollisionPos`/`setWolfCollisionPos` (stock 6698/6626) | **Mod-hookable** (§6.4) |
| OC/AC/AT colliders re-set per frame + custom `DamageTable` | Puppet colliders are set up by `playerInit`; TP's `dCcS` sweep is global/automatic — the puppet just repositions them. Custom hit response: replace-hook `daAlink_c::setDamagePoint` (damage.inc:177), or install custom `SetTgHitCallback`/`SetCoHitCallback` (d_cc_d.h) on the puppet's cylinders | **Mod-hookable** (§6.3) |
| `gSaveContext` juggling in Init *and* Draw | Create-time juggle only; `checkWolf()` is instance state (d_a_player.h:1077), draw reads per-instance fields | **Less juggling than OoT** |
| `DummyPlayer_Destroy` resets `actor->id` (numLoaded) | Nothing to reset; no counters; `~daAlink_c` clears slot-0 status flags (harmless) | **Not needed** |
| Off-scene: teleport to (-9999), `shadowAlpha = 0` | Puppet lives on the play-scene layer and survives room changes; hide via the custom Draw returning early (or move off-screen) when the remote player's room ≠ local room | **Mod-side** |

**Net assessment:** Anchor's pattern is *directly portable* in spirit, and TP is friendlier in
three places (no category/id swap needed, no `numLoaded`, less save juggling) and only
different in one (no lazy-init window — the create replace-hook is the hijack point, and the
`sub_method` swap makes the per-instance method replacement exact).

---

## 6. Driving a puppet: freeze + apply

Two architectures, both pure-mod:

- **Option A — shared replace-hooks** (minimal surface): replace-hook `daAlink_c::execute`
  and `daAlink_Draw`; for puppet instances run the apply+pose path and return without calling
  the original. Zero instance mutation beyond the pid registry. Simple; every puppet/real
  Link call goes through the trampoline.
- **Option B — `sub_method` swap (Anchor-faithful, recommended)**:
  1. Mod sets `spawningPuppetForClientId` before `fopAcM_create(ALINK, …)`.
  2. Replace-hook `daAlink_c::create`: on the phase call that returns `cPhs_COMPLEATE_e` for
     the puppet, write `actor->sub_method = &g_puppetAlinkMethod`, where
     `g_puppetAlinkMethod` is a mod-defined `actor_method_class`
     `{ puppetCreate(stub), puppetDelete, puppetExecute, NULL, puppetDraw }` matching
     `l_daAlink_Method`'s layout (§5.2). The framework's execute-queue entry
     (`fpcCtRq_Do` → `fpcEx_ToExecuteQ`) happens *after* create returns, so the first
     execute/draw already use the custom table. The vanilla `daAlink_Execute`/`daAlink_Draw`
     are never called for the puppet; the real Link is untouched.
  3. `puppetExecute` = the Anchor `DummyPlayer_Update` analog; `puppetDraw` = the draw
     analog; `puppetDelete` = bookkeeping + `fopAcM_delete` semantics.

Option B's `sub_method` write is a plain data-field write on the actor (mod links the binary;
`f_op_actor.h:18` is a pointer field, writable despite `DUSK_CONST` on the pointee). §6.2/§6.4
apply to both options; the hook list in §7 is for Option A with Option-B deltas noted.

### 6.1 Spawn
1. `fpcLy_SetCurrentLayer(&playScene->layer)` around the create, where
   `playScene = fpcM_SearchByName(fpcNm_PLAY_SCENE_e)` — copy `coop_player.cpp:338-350`.
2. `fpc_ProcID pid = fopAcM_create(fpcNm_ALINK_e, 0xFFFF, 0, &pos, roomNo, &angle, nullptr, -1, nullptr);`
   (public, `f_op_actor_mng.cpp:255`). No duplicate-name gate. Keep `pid` in the mod registry
   (Option B: set `spawningPuppetForClientId` first).
3. Wait for `daAlink_c::create()` to reach `cPhs_COMPLEATE_e` (multi-frame; poll
   `fopAcM_SearchByID(pid)` + `fopAcM_GetName(actor) == fpcNm_ALINK_e`, exactly like
   `coop_player.cpp:370-376`). On completion: Option B installs `sub_method` here (inside the
   create replace-hook); the mod marks the actor as puppet in the registry.

### 6.2 Create-time neutralization (replace-hook `daAlink_c::create`)
For the puppet instance (identified by pid / `spawningPuppetForClientId`):
- Set a `puppetCreateInProgress` flag; snapshot `mPlayerInfo[0].mpPlayer` and
  `mPlayerPtr[0]`; call `g_orig(...)`; restore the two pointers; clear the flag. Repeat on
  every phase call (create is called per frame until complete). `g_orig`'s return value is
  returned unchanged.
- While the flag is set, suppress the create's spawn fan-out by pre-hooking the create entry
  points (`fopAcM_create`, `fopAcM_fastCreate` — real symbols at f_op_actor_mng.cpp:255/273;
  the inline `fopAcM_Create`/`fopAcM_FastCreate` route through them) and returning
  `HOOK_SKIP_ORIGINAL` (post-hook writes `retval = fpcM_ERROR_PROCESS_ID_e`) for
  `fpcNm_MIDNA_e`, `fpcNm_HORSE_e`, `fpcNm_NPC_TK` (from `checkSetNpcTks`), and the portal
  warp-miss tag. Alternatively let them spawn and `fopAcM_delete` them after create — the
  suppression approach is cleaner.
- Suppress the save write: pre-hook `dComIfGs_setRestartRoom` while the flag is set.
- **Form/wear juggling (Anchor §5.4):** if the puppet must not inherit the host's story
  form/wear, save the `dSv_info_c` fields used by create (`dComIfGs_Wolf_Change_Check`,
  `dComIfGs_getSelectEquipClothes`), force "human + default wear", call `g_orig`, restore.
  This only affects which arc/model loads; all later form/wear changes are per-instance
  (`checkWolf()` = `FLG1_IS_WOLF` instance bit; `mEquipItem`/hand indices are instance
  fields) and driven from the packet. Simpler v1: skip the juggle, accept host form at spawn,
  `changeLink(0)`/`changeWolf()` if the received form differs.
- Accept that `setStartProcInit()` runs with host save data — the mod overrides everything
  post-create from the network packet (§6.4).

### 6.3 Freeze + damage suppression
Option A:
```cpp
int on_execute_replace(ModContext*, void* args, void* retval, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    if (is_puppet(link)) {
        int r = puppet_apply_state(link);   // §6.4
        if (retval) *static_cast<int*>(retval) = r;
        return;
    }
    int r = LinkExecute::g_orig(link);
    if (retval) *static_cast<int*>(retval) = r;
}
```
This skips the entire state machine: `setStickData` (no input), `posMove` (d_a_alink.cpp:13000 —
no physics), damage/free fall, demo checks, death/game-over procs, HUD writes. Also replace
`daAlink_c::posMove` (defensive no-op for puppets).

Option B needs no execute hook at all — `g_puppetAlinkMethod.execute = puppetExecute`
replaces it wholesale.

Damage suppression (required, §2.7):
- Replace-hook `daAlink_c::setDamagePoint` (damage.inc:177) — skip for puppets (protects host
  save + HP). Also skip `setDamagePointNormal` (228) and `setLandDamagePoint` (232).
- Optional: pre-hook the file-local statics `daAlink_tgHitCallback`/`daAlink_coHitCallback`
  (d_a_alink.cpp:104/109) to drop hits on the puppet entirely. Anchor's custom `DamageTable`
  maps to either these hooks or a mod-installed `SetTgHitCallback` on the puppet's cylinders
  (d_cc_d.h) for PvP responses.

Draw-time fix (required for correct rendering of 2+ Links, both options):
- Replace-hook `daAlink_c::modelCalc` (stock 19364): before `g_orig(i_model)`, re-assert
  `md->getJointNodePointer(...)->setMtxCalc(field_0x1f20/field_0x1f24)` on the shared
  `J3DModelData` for the puppet's `mpLinkModel` (joints 0/1/16 human, 0/3/15 wolf — mirror
  fork d_a_alink.cpp:19875-19892). Without this, the second Link to initialize overwrites the
  mtxCalc pointer for both, and the first Link animates the puppet's (or vice-versa) pose.
- `daAlink_Draw` (19862) → `draw()` (19466) stays vanilla in Option A (or is the mod's
  `puppetDraw` in Option B): the puppet renders through the normal actor draw pipeline with
  its frozen pose.

### 6.4 Apply state (in the puppet's update path, per received network snapshot)
Write (all public; `include/d/actor/d_a_alink.h`):
- **Transform:** `current.pos`, `current.angle`, `shape_angle`, `speed`, `mNormalSpeed`,
  `mMaxSpeed`. Also `old.pos` if interpolation is wanted.
- **Action:** `mProcID` (+ `mProcVar0..4` if carrying proc-specific state). v1: always
  `PROC_WAIT`.
- **Animation — pose surface (§5.3):** either
  (i) anm-driven: `setSingleAnimeBase(anmIdx)`, `setUpperAnimeParam(...)`,
  `setBlendMoveAnime(morf)` or direct `mUnderFrameCtrl[i].setFrameCtrl(attr, start, end, rate,
  frame)` writes, then `setMatrix()` (5744) + `allAnimePlay()` (7258) + `mpLinkModel->calc()`;
  or
  (ii) matrix-copy: `setMatrix()`, run `calc()`, then copy the received per-joint `Mtx`
  table via `getAnmMtx(j)`/`setAnmMtx(j, mtx)` (J3DModel.h:109-116).
- **Form:** `onNoResetFlg1(FLG1_IS_WOLF)` / `offNoResetFlg1(FLG1_IS_WOLF)` — drives
  `checkWolf()` (d_a_player.h:1077); call `changeLink(0)`/`changeWolf()` when the form itself
  must swap models.
- **Equipment/pose extras:** `mEquipItem`, `mLeftHandIndex`/`mRightHandIndex`, `setItemMatrix(0)`
  / `setWolfItemMatrix()`.
- **Collision (Anchor's per-frame OC/AC/AT registration, TP style):** `mLinkAcch.CrrPos(dComIfG_Bgsp())`,
  `setRoomInfo()`, and `setCollisionPos()` (6698) / `setWolfCollisionPos()` (6626) so enemy
  attacks hit the puppet at its real location and the room no stays correct. TP's `dCcS`
  sweep is global — the colliders only need repositioning, not re-registration.
- **Pose computation order (v1):** transform → form flag → mProcID → anims → `setMatrix` →
  `allAnimePlay` → `mpLinkModel->calc` → `setCollisionPos`/`setWolfCollisionPos` →
  `mLinkAcch.CrrPos` → `setRoomInfo` → `setBodyPartPos` → `setAttentionPos`.
- **Hidden state (Anchor's off-scene handling):** when the remote player's room ≠ local room,
  the puppet update can skip pose work and the (custom) draw returns early.

### 6.5 Despawn
- `fopAcM_delete(actor)` (public, f_op_actor_mng.h:539) or `fopAcM_delete(pid)`. Option B's
  `puppetDelete` runs through the custom table; no `id` reset needed (§2.9).
- On stage change: hook `dScnPly_Delete` (d_s_play.cpp stock 836, file-local static —
  `DEFINE_HOOK_SYMBOL`) to drop the puppet registry entry; re-spawn in `mod_update` once the
  new stage's own Link exists (or hook `dScnPly_Create`/post-hook `dStage_playerInit`).
  Polling (`fopAcM_SearchByID(pid) == NULL` → re-spawn) is the fork's fallback
  (`recreatePendingLinks`, coop_player.cpp:247) and works without any stage hook.
- Puppet death from the network (player left/DC): delete the actor, keep the slot for rejoin
  (per `docs/design/network.md` §10).

---

## 7. Top risks

1. **Global pointer clobber during create (HIGH).** If the §2.1 restore is missed even one
   frame, the camera chases the puppet and enemies/NPCs/demos treat it as the hero; the real
   Link becomes an orphan. Mitigation: restore in the same replace-hook that calls `g_orig`,
   plus a frame-boundary assert in `mod_update` that slot 0 still points at the real Link.
2. **Shared J3DModelData mtxCalc (HIGH).** Without the §6.3 `modelCalc` fix, the two Links
   render each other's animation (last writer wins). Must be in the first playable build.
3. **Host save corruption (HIGH).** `dComIfGs_setRestartRoom` (create) and
   `setDamagePoint`/`dComIfGp_setItemLifeCount` (hits) mutate the host's save/hearts. Both
   hooks are mandatory before any playtesting.
4. **Midna/NPC/horse fan-out (MEDIUM).** A second Link's create spawns a second Midna (which
   follows slot 0 = the real Link) plus NPCs; the fork gates these to the story authority.
   Suppress while `puppetCreateInProgress` (§6.2).
5. **Cutscene freeze-frame puppets (MEDIUM, cosmetic).** Demos run on the real Link; puppets
   stand still in the middle of scripted events. Acceptable per `network.md` §5/§8
   (per-triggerer demos, no replication). Possible later polish: pose puppets toward the
   camera subject during demos (Anchor-style cutscene awareness is a known gap there too).
6. **Room-clear / door gating (MEDIUM).** `dStage_RoomCheck` runs per Link via `setRoomInfo`;
   the puppet's ground poly contributes. Keep the puppet's Acch current (§6.4) so it doesn't
   wedge doors.
7. **Form/resource mismatch (MEDIUM).** The puppet's loaded model set is fixed at create by
   the host save's wolf state (or by the §6.2 juggle); receiving a different form requires
   `changeWolf`/`changeLink` model swaps (public, but heavier). v1: puppet spawns in a forced
   form and form changes are applied via `changeWolf`/`changeLink`.
8. **PvP combat reach (LOW for v1).** Collision hits still register (puppet keeps colliders),
   so host enemies accidentally hitting a puppet is handled via `setDamagePoint` suppression;
   making *players* fight each other = damage-intent packets at the collision boundary
   (Anchor's model), needs the `dCcS`-level FF filter the fork has in-tree — defer.
9. **Horses (LOW).** `dComIfGp_getHorseActor()` = `mPlayerPtr[1]` (host horse). Suppress horse
   spawn at puppet create (§6.2). "Remote player rides horse" is future work: spawn a
   `fpcNm_HORSE_e` per remote player (same `fopAcM_create` pattern — the fork does it in
   `coop_horses.cpp:263`) and puppet-drive it the same way.
10. **`fopAc_Execute` NPC context (N/A).** The fork's NPC-scoping patch (`f_op_actor.cpp:293`
    fork) is only needed when NPCs must talk to the *nearest* of several in-process Links.
    Single-machine puppets don't need it — NPCs correctly talk to the real Link.
11. **Debug HIO per-actor controls (LOW, debug-only).** "Stop ALINK" in the HIO stops all
    Links (procname match). Debug tooling only; no release impact.
12. **Culling (LOW).** The standard `fopAcStts_CULL_e` culling path applies to the puppet like
    any actor; it only skips draw, which is fine (the frozen update still runs).

---

## 8. Step-by-step implementation plan (puppet lifecycle)

**Spawn**
1. On "remote player entered room" packet: resolve spawn pos/angle (mirror
   `tryPlaceNearAuthority` logic; v1: host's restart-point-derived pos + a small offset).
2. `fpcLy_SetCurrentLayer(playScene->layer)` → set `spawningPuppetForClientId` (Option B) →
   `fopAcM_create(fpcNm_ALINK_e, 0xFFFF, 0, &pos, roomNo, &angle, nullptr, -1, nullptr)` →
   restore layer. Store `pid`.
3. In `mod_update`, poll `fopAcM_SearchByID(pid)`; on first non-null ALINK actor: mark
   `isPuppet` in the registry; on the create-complete phase, Option B installs
   `actor->sub_method = &g_puppetAlinkMethod` (inside the create replace-hook).

**Freeze**
4. Option A: replace-hook `daAlink_c::execute` (+ defensive `posMove`); Option B: covered by
   the custom table's `puppetExecute`.
5. Replace-hook `daAlink_c::setDamagePoint` (+ `setDamagePointNormal`, `setLandDamagePoint`):
   puppets skip. Pre-hook `daAlink_tgHitCallback`/`daAlink_coHitCallback` to drop hits
   (optional v1; required for PvP, where they become intent senders instead).
6. Replace-hook `daAlink_c::modelCalc` with the §6.3 mtxCalc re-assert for every Link on the
   shared model data.

**Apply state**
7. Every received snapshot (20-30 Hz, interpolate between): write §6.4 fields; run the pose
   pipeline (setMatrix → allAnimePlay → model calc [→ matrix copy] → colliders → Acch →
   setRoomInfo → body parts → attention).
8. v1: map packet (action, anim, frame) → `mProcID` + `setSingleAnimeBase`/`setUpperAnimeParam`
   table; fall back to `PROC_WAIT` + idle anim for unmapped states. Upgrade to matrix-copy
   (§5.3-ii) when exact pose fidelity matters.

**Despawn**
9. Leave/DC packet or `fopAcM_SearchByID(pid) == NULL` (stage changed): `fopAcM_delete(actor)`,
   clear registry; re-enter spawn flow for rejoin.
10. Hook `dScnPly_Delete` (and optionally `dScnRoom_Delete`) to drop puppet bookkeeping
    before the stage heap goes away (mirrors fork `d_s_play.cpp:862`).

**Hooks needed (complete list; Option B replaces the execute/draw entries with the
`sub_method` table)**
- `daAlink_c::create` — replace (pointer save/restore, flag, Option-B table install) —
  `d_a_alink.cpp:4887`
- `daAlink_c::execute` — replace (freeze/apply; Option A only) — `d_a_alink.cpp:17798`
- `daAlink_c::posMove` — replace (defensive) — `d_a_alink.cpp:13000`
- `daAlink_c::modelCalc` — replace (mtxCalc) — `d_a_alink.cpp:19364`
- `daAlink_c::setDamagePoint` (+ Normal/Land) — replace — `d_a_alink_damage.inc:177/228/232`
- `daAlink_tgHitCallback` / `daAlink_coHitCallback` — pre (hit drop / intent) —
  `d_a_alink.cpp:104/109`
- `fopAcM_create` / `fopAcM_fastCreate` — pre (spawn suppression while flag set) —
  `f_op_actor_mng.cpp:255/273`
- `dComIfGs_setRestartRoom` — pre (suppress while flag set)
- `dScnPly_Delete` / `dScnRoom_Delete` — pre/post (bookkeeping) — `d_s_play.cpp:836`,
  `d_s_room.cpp:325`

Direct calls (no hook): `fopAcM_create(ALINK)`, `fpcLy_SetCurrentLayer`,
`fopAcM_SearchByID`, `fopAcM_delete`, `daAlink_c` pose members (§6.4),
`changeLink/changeWolf`, `J3DModel::setAnmMtx/getAnmMtx/setBaseTRMtx`.

---

## 9. What looks impossible from a pure .dusk mod

- **Per-actor "current Link" context** (the fork's `ScopedContext` + `playerForContextBridge`
  inline re-routing): impossible — requires header/source edits; inlines are compiled into
  stock callers and the play struct is ABI-fixed. **Not needed** for the networked puppet.
- **Multi-viewport / split-screen rendering** (fork `coop_render.cpp`, `mWindow[MAX_VIEWS]`):
  impossible from a mod (struct growth + render pipeline edits). **Not needed** — the networked
  model is one camera per machine.
- **Injecting code in the middle of vanilla functions** (e.g. the fork's indexed-input branch
  inside `setStickData`): impossible directly; a replace-hook must reimplement the whole
  function or a pre/post boundary must suffice. For the puppet this is fine (freeze = skip;
  apply = outside calls; `sub_method` swap = replace at the dispatch boundary).
- **Enemy AI retargeting to puppets in general**: impractical — target access is inline in
  100+ actor files. Only the collision layer is mod-reachable. Accept host enemies aggroing
  the host Link; validate puppet hits at the `dCcS`/damage boundary instead (Anchor's model).
- **Changing native `mPlayerInfo`/`mPlayerPtr`/`mWindow`/`mCameraInfo` sizes**: impossible —
  ABI. Puppet lives only in the mod registry, never in native slot 0 (restored after create).
- **`fopAcM_fastCreate` for ALINK**: not usable (multi-phase create bails synchronously) —
  use async `fopAcM_create` + poll. Not "impossible", just the wrong API.

**Bottom line:** spawning, freezing, driving, damaging and despawning a puppet `daAlink_c` is
fully achievable as a pure `.dusk` mod on stock Dusklight. The Anchor pattern transfers cleanly:
its hijack-before-init maps to TP's create replace-hook (TP has no lazy-init, so the mod runs
*before* any global pollution), its per-actor method replacement maps exactly to writing
`actor->sub_method`, its `skelAnime.jointTable` rotation copy maps to TP's matrix-driven
`J3DModel` `setAnmMtx`/`getAnmMtx`, and three of its hacks (category move, `numLoaded` reset,
heavy `gSaveContext` juggling) have no TP equivalent because the engine is structurally
friendlier. The required hook set is small (~10 targets, all symbol-manifest-visible), and
every hard problem the fork solved in-tree (per-player context, split-screen) is exactly the
machinery the networked model intentionally does not need.
