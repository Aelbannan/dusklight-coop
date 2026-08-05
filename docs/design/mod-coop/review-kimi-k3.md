# Adversarial review — networked co-op mod implementation plan

Reviewer: kimi-k3 (adversarial pass over `implementation-plan.md` + sources 00–04 + `docs/design/network.md`).
Method: every load-bearing claim spot-checked against the actual source. "Stock" claims were checked
against `upstream/main` (TwilitRealm/dusklight @ 2c9f814e) via `git show`; SDK/binary claims against
`sdk/`, `src/dusk/mods/`, and the built binary `build/macos-default-relwithdebinfo/Dusklight.app`
(129,973 symbols, `__symdbh` manifest section present, ~4,966 file-local static symbols in the symtab).

## VERDICT

**Buildable with caveats — not buildable as literally specified.**

The core architecture survives adversarial scrutiny: the spawn path, the `sub_method` freeze, the
matrix-copy pose (including frame-interp interaction), the `fopAc_Execute` enemy freeze, the combat
intercept points, and every time/weather symbol and struct offset checked out as **real and correct**.
Nothing found requires an engine fork; the "pure `.dusk` mod" thesis holds.

But the plan as written contains: **one guaranteed crash on a normal platform operation** (mod
disable/reload with live puppets), **one impossible hook target** (an inline function listed as a
mandatory M1 hook), and **four design holes** that would each fail their own milestone acceptance
criteria (puppet collider registration, client-side drops, weather ramp semantics, concurrent puppet
creates). All are fixable without architecture changes. The investigations' *line numbers* drift
(±2–33 lines vs. current upstream) but their *content* was right in every case I checked.

---

## RANKED FINDINGS

### BLOCKER 1 — Mod unload/reload with live puppets crashes the game; the plan never addresses lifecycle

**Claim:** M0 accept: "mod loads, hooks fire, **reload works**." The plan has no teardown story for
spawned actors anywhere (M1–M4, risk register, §3 cross-cutting rules — all silent).

**Evidence:**
- The puppet drive mechanism writes `actor->sub_method = &g_puppetAlinkMethod` — a table of function
  pointers **into the mod's own library** (plan §5 M1 "Freeze"; 01 §6 Option B).
- `docs/modding.md` "Runtime Lifecycle": disable/reload **unloads the library** ("removes your hooks,
  services, overlays… and unloads your library"), applied between frames.
- After unload, every live puppet's next `fopAc_Execute`/`fopAc_Draw`/`fopAc_Delete` dispatches
  through a dangling pointer (`fpcMtd_Execute(actor->sub_method, actor)`, `src/f_op/f_op_actor.cpp:370-371`,
  draw at :264, delete at :443) → jump into unmapped memory → crash. Same for the enemy whitelist
  state and the SPSC rings/net thread (the plan does cover the thread join in `mod_shutdown`, but not
  the actors).

**Why it breaks the plan:** the very first Disable/Reload after M1 ships crashes the process. This is
not an edge case — mod reloading is a first-class platform feature users will hit mid-session.

**Fix:** `mod_shutdown` must, before returning: (a) `fopAcM_delete()` every puppet (player + enemy)
in the registry, or (b) restore each puppet's `sub_method` to the vanilla table (`l_daAlink_Method`
is resolvable: it's a local data symbol, confirmed in the binary `s __ZL16l_daAlink_Method`; the
enemy equivalents likewise). (a) is cleaner. Add "disable/reload with an active session and spawned
puppets leaves zero mod-owned state and does not crash" to M1 acceptance. Note hooks are safely
auto-removed by the loader; actors are not.

---

### MAJOR 1 — `dComIfGs_setRestartRoom` is a header inline; the mandatory M1 hook cannot exist

**Claim:** Plan §5 M1 hook list and 01 §6.2/§7/§8: "`dComIfGs_setRestartRoom` (pre, suppress)" —
listed as one of the mandatory anti-save-corruption hooks (risk #3).

**Evidence:** `include/d/d_com_inf_game.h:2511`:
```cpp
inline void dComIfGs_setRestartRoom(const cXyz& i_position, s16 i_angle, s8 i_roomNo) {
    g_dComIfG_gameInfo.info.getRestart().setRoom(i_position, i_angle, i_roomNo);
}
```
There is **no symbol** — confirmed absent from the built binary (`nm` finds nothing; the call inside
`daAlink_c::create` at upstream `d_a_alink.cpp:5031` is compiled inline). The modding docs are
explicit that inlines "compile into every caller" and cannot be hooked.

**Why it breaks the plan:** the hook list is the M1 contract; one of its three mandatory
save-protection hooks is impossible as written. (Discovered because the same investigation correctly
flags *other* inlines as unhookable — it missed this one.)

**Fix (trivial):** the inline's callee **is** a real symbol — `dSv_restart_c::setRoom`
(`__ZN13dSv_restart_c7setRoomERK4cXyzsa`, confirmed in binary; declared `include/d/d_save.h:832`).
Pre-hook that during `puppetCreateInProgress`, or save/restore the restart-room fields around
`g_orig` in the create replace-hook (same pattern as the slot-0 pointers). Also note the create has
**other** save writes nobody listed: `dComIfGs_setSelectEquipClothes(...)` under
`checkCasualWearFlg() && isEventBit(47)` (upstream `d_a_alink.cpp:~4916`) mutates the host's equipped
clothes on a puppet create — fold into the save/restore set.

---

### MAJOR 2 — Concurrent puppet creates race on a stock file-static; the plan's single spawn flag has the same flaw

**Claim (implicit):** the spawn flow (01 §6.1, plan §5 M1) can create puppets whenever remotes join —
e.g. WorldInit with a 4-player roster spawns 3 puppets, plausibly in the same frame(s).

**Evidence:** stock `daAlink_c::create` keeps its multi-phase state in **one file-local static shared
by all Links**: `static BOOL bgWaitFlg = FALSE;` (upstream `d_a_alink.cpp:4890`), reset to FALSE at
create completion (:5029). Two in-flight creates stomp each other's phase state (a completion resets
the flag mid-way through the other create → phase-1 re-runs → double `playerInit()`, double heap
entry, double resource requests). The fork hit exactly this and replaced the static with per-slot
state (`bgWaitFlag(coopOwner)`, fork `d_a_alink.cpp:4924-4929`) — a mod cannot do that. The plan's
own `spawningPuppetForClientId` / `puppetCreateInProgress` flags (01 §6.1–6.2) are likewise single
slots and corrupt under two concurrent creates.

**Why it breaks the plan:** two remote players joining within the same multi-frame create window
(common: session start, mass rejoin after stage change) produces a corrupt or half-initialized
puppet, non-deterministically.

**Fix:** serialize puppet creation — one in flight, queue the rest (a `std::queue` in the registry;
spawn the next when the current pid resolves). One sentence of design; must be written down because
the natural implementation spawns them all at once.

---

### MAJOR 3 — The player puppet is intangible: collider re-registration is missing from the apply pipeline

**Claim:** 01 §5.6/§6.4: "OC/AC/AT colliders re-set per frame … the puppet just repositions them …
so enemy attacks hit the puppet at its real location"; plan §5 M1 Apply list ends at
`setCollisionPos()`/`setWolfCollisionPos()`; risk table and Anchor comparison both claim the puppet
"keeps collision … participates in combat".

**Evidence:**
- `cCcS` registrations are **per-frame**: `cCcS::Move()` ends by zeroing all object counts
  (`src/SSystem/SComponent/c_cc_s.cpp:465-484`). Any actor that doesn't re-`Set()` every frame has
  no collision.
- `daAlink_c::setCollisionPos()` only repositions (`SetC`/`SetH` on `mTgCyls`, upstream
  `d_a_alink.cpp:6698+`) — no registration. Registration lives in the execute-time collision
  function (`dComIfG_Ccsp()->Set(&mTgCyls[i])` + `SetMass`, upstream :6848-6872, gated on
  `mDamageTimer`/mode flags) which the frozen puppet **never runs** under the plan's chosen Option B
  (`sub_method` swap — vanilla execute is never dispatched).
- 02 §4 assumed the opposite ("The vanilla `execute` still runs fully (zeroed input) … colliders
  stay live for local hit detection") — the plan rejected that design (§5 "Resolution of the 01-vs-02
  conflict") but silently kept its collider consequences.

**Why it breaks the plan:** as specified, puppet Tg cylinders are registered only during create and
vanish on the first `cCcS::Move()`. Host enemies can never hit a puppet (not even to be suppressed),
there is no hit feedback, and the M1 accept criterion's spirit (Anchor-faithful dummy with live
collision) is unmet. This is the concrete residue of the 01-vs-02 conflict resolution the plan
claims to have fully absorbed.

**Fix:** either (a) add per-frame registration to `puppetExecute` — `OnTgSetBit()` +
`dComIfG_Ccsp()->Set(&mTgCyls[i])` + `SetMass(...,1)` for i=0..2 after `setCollisionPos()`
(replicating upstream :6848-6872, all public) — or (b) consciously declare puppets intangible and
strike the collision claims from 01 §5.6/§6.4 and the plan. (a) is one-to-five lines and matches the
stated design. Note the enemy side is fine: the adapter's `refreshColliders` (e.g.
`e_ai_class::setCcCylinder`) does register (`dComIfG_Ccsp()->Set(&m_ccCyl)`,
`src/d/actor/d_a_e_ai.cpp` — verified).

---

### MAJOR 4 — Client-side drops: the "gate the native roll" design is a no-op on frozen puppets; nothing spawns drops on clients

**Claim:** Plan §5 M2: "on `EnemyEvent(died)`, gate local drop creation via hooks on
`fopAcM_createItemFromEnemyID`/`createItemFromTable` — only confirmed kills roll." 03 §4.5: "lets the
drop roll proceed only for kills it confirmed … since on the client the killing blow never passes
through the local damage path." M2 accept: "drop spawns on both."

**Evidence:** enemy drops are created in the enemy's **execute-time death actions**, never in the
delete/destructor path — e.g. `fopAcM_createItemFromEnemyID(field_0x564, &current.pos, …)` in
`d_a_e_fz.cpp:521`, `d_a_e_gm.cpp:399`, `d_a_e_bs.cpp:467`. On a client, the enemy puppet is frozen
(`fopAc_Execute` pre-hook skip) and on `EnemyEvent(died)` the plan **deletes** it
(`fopAcM_delete`, plan §5 M2 room-clear). The death action never runs → **no native drop-creation
call ever happens on the client** → there is nothing to "gate". The hooks as specified will simply
never fire for puppet deaths.

**Why it breaks the plan:** M2's accept criterion "drop spawns on both" fails; clients get no drops,
ever. The 03 author was mentally in a "remote HP on a live actor" model, not the freeze model the
plan adopted.

**Fix:** invert it. On `EnemyEvent(died)`, the client mod **explicitly spawns** the drop:
read the puppet's drop-table id (per-type field, e.g. `field_0x564`, carried by the adapter) and
call the public `fopAcM_createItemFromEnemyID(id, &pos, -1, -1, …)` (symbol verified:
`__Z28fopAcM_createItemFromEnemyIDhPK4cXyziiPK5csXyzS1_PfS5_`) before `fopAcM_delete`. Host side
needs no gating (its kills are all authoritative). Related presentation gap: delete-on-died makes
enemies **pop out of existence** on clients — no death action, no burst VFX (all spawned by the
death code). Adapter should drive a death anim (`m_action`/`m_modelMorf` frame) + effect for a beat
before deletion, or the game feels broken.

---

### MAJOR 5 — Weather client design is self-contradictory: pinning intensity every frame precludes the local ramp; the suppressed dice machine was the ramp

**Claim:** Plan §5 M3 (client): "pre-`exeKankyo` force `dKyw_rain_set(intensity)`/`mSnowCount`/
`mThunderEff.mMode`/`dKy_change_colpat(colpat)`; **re-run the vanilla ramp locally between
changes**; suppress the local dice machine (`daKytag06_Draw`, skip type 4)."

**Evidence:** the "vanilla ramp" *is* the dice machine — `dKy_event_proc` ramps `raincnt` ±1-3/frame
toward the mode target, invoked only from `daKytag06_Draw` when `mType == 4`
(`src/d/actor/d_a_kytag06.cpp:290-294` → `daKytag06_type_04_Execute` :258-261 → `dKy_event_proc`
:63+), which the same paragraph suppresses. 04 §4.3 itself says `dKy_event_proc` is inlined away and
must be **replicated**, not hooked. If the mod pins `raincnt` via `dKyw_rain_set(intensity)` every
frame pre-`exeKankyo`, any local ramp is instantly overwritten; if it doesn't pin, nothing moves the
value (dice suppressed) and shelter tags (kytag00) cap/restore against a stale `base_raincnt`.

**Why it breaks the plan:** as written, rain intensity either steps discontinuously on each
`WeatherChange` (pin-every-frame reading) or never changes at all (suppress-dice reading). Two
implementers reading the same paragraph will build different, both-wrong, clients.

**Fix:** state it precisely: on `WeatherChange` the client sets a local *target* + colpat/thunder,
and a **mod-implemented ramp** (replicating the dice proc's ±1-3/frame rule toward the mode target,
40 ≈ light / 250 ≈ heavy per 04 §4.3 table) advances `raincnt` each pre-`exeKankyo`; re-pin only on
packet receipt. kytag00 shelter tags then modulate locally exactly as on the host. Minor adjacent
cosmetic: thunder flash *timing* is random per machine (mThunderEff lifecycle) — flashes won't
coincide; acceptable, but say so.

---

### MAJOR 6 — Fork/stock ABI hazard is real and unprotected: the fork grew game structs without bumping the ABI epoch; the M1 "loopback parity in the fork" test requires the dangerous configuration

**Claim:** Plan §2: "Built against **stock** SDK headers pinned to a `DUSKLIGHT_VERSION` (GameService
ABI epoch must match the runtime binary; do not build against the fork's `TARGET_PC` headers)." M1
accept: "loopback parity test in the fork's split-screen during dev."

**Evidence:**
- The fork did grow game-visible structs: `git diff upstream/main -- include/d/d_com_inf_game.h`
  shows `mPlayerInfo[1]→[MAX_PLAYERS]`, `mPlayerPtr[2]→[MAX_PLAYERS][2]`,
  `mPlayerStatus[1][4]→[MAX_PLAYERS][4]`, `mWindow[1]→[MAX_VIEWS]`, `mCameraInfo[1]→[MAX_VIEWS]`
  (343-line diff).
- The epoch was **not** bumped: `GAME_SERVICE_MAJOR 1u` in both `sdk/include/mods/svc/game.h`
  (fork) and upstream's. The header comment says the major version "is bumped when game-visible
  struct or vtable layouts change incompatibly (e.g. a TARGET_PC field added to an existing game
  struct)" — precisely what the fork did.
- The SDK compiles mods against the repo's own `include/` tree (`cmake/GameABIConfig.cmake`:
  `_game_abi_include_dirs` = `${_game_root}/include`, …). A mod built in *this* repo gets fork
  layouts.
- Consequence: a fork-header build of the coop mod **loads cleanly into stock** (epoch matches)
  and then writes `mPlayerInfo[0].mpPlayer` save/restore at the wrong offsets → silent memory
  corruption. And the M1 loopback dev test *requires* a fork-header build (the stock build has no
  split-screen harness), creating exactly the artifact that must never be distributed.

**Why it breaks the plan:** the plan's only guard is a discipline ("do not build against the fork's
headers") that the build system does not enforce and the runtime cannot detect.

**Fix:** (1) bump `GAME_SERVICE_MAJOR` in the fork (one-line, protects every future mod too);
(2) the coop mod must live in a standalone repo / build tree rooted at upstream `@
DUSKLIGHT_VERSION` (the plan already says "or standalone repo" — make it mandatory, not optional);
(3) re-scope the M1 loopback item: either drop it or mark it explicitly "requires a fork-ABI build
artifact, never distributable, delete after use."

---

### MINOR 1 — Overloaded hook display names will fail with `MOD_CONFLICT`

`fopAcM_create` has two C++ overloads (upstream `f_op_actor_mng.cpp:255` and :267; binary has both
mangled symbols), `fopAcM_fastCreate` has three, `fopAcM_delete` two. `docs/modding.md` "Hooking by
name": overload sets via display name return `MOD_CONFLICT`. The plan/01 hook lists name these as
bare display names. **Fix:** use the mangled spellings
(`__Z13fopAcM_createstjPK4cXyziPK5csXyzS1_aPFiPvE`, etc.) everywhere; add a resolve-all-targets
self-check at mod init in M0 so every planned hook name is proven against the manifest before any
gameplay code exists.

### MINOR 2 — Spawn-suppression blacklist is incomplete; make it suppress-all

01 §6.2/plan M1 suppress `fpcNm_MIDNA_e`, `fpcNm_HORSE_e`, `fpcNm_NPC_TK` + "the portal warp-miss
tag". Stock create's actual fan-out: MIDNA + `checkSetNpcTks` + portal tag (upstream
`d_a_alink.cpp:5104-5108`) **and** `fopAcM_create(fpcNm_CANOE_e, …)` / `fopAcM_create(fpcNm_Obj_IceLeaf_e, …)`
ride-actor spawns under special scene modes (:4999-5002) plus whatever `setStartProcInit` triggers.
No horse is created by `daAlink_c::create` at all (the `fpcNm_HORSE_e` entry guards nothing — stage
data spawns horses). **Fix:** while `puppetCreateInProgress`, skip **all**
`fopAcM_create`/`fopAcM_fastCreate` calls (nothing a puppet create spawns is wanted). Blacklisting
proc names is fragile against exactly this kind of omission.

### MINOR 3 — Doc imprecision: the `sub_method` write target

01 §5.2 cites "`sub_method` (`f_op_actor.h:18`, `/* 0x24 */ DUSK_CONST actor_method_class*
sub_method`)". That line/offset is the **profile** field (`actor_process_profile_definition::sub_method`).
The per-instance field a mod writes is `fopAc_ac_c::sub_method` at **0x0EC** (`f_op_actor.h:282`,
opaquely typed `profile_method_class*`, assigned from the profile once at `f_op_actor.cpp:480` under
`fpcM_IsFirstCreating`). Design unaffected — the mechanism verified end-to-end (assignment once at
first create phase; execute/draw/is-delete/delete dispatch through it at
`f_op_actor.cpp:370/264/421/443`; create completes before execute-queue entry,
`f_pc_create_req.cpp:~105` + `f_pc_executor.cpp:33`; NULL `is_delete` returns 1,
`f_pc_method.cpp`) — but the doc sends an implementer to the wrong struct member.

### MINOR 4 — Enemy snapshot cadence contradiction is resolved in the plan but not in the sources

00-network.md §5's table says `EnemySnapshot` "every frame" and §6 says "Enemy snapshots: same
every-frame policy"; the plan decides 30 Hz. `docs/design/network.md` §4 says 20–30 Hz for both
players and enemies while the plan says players every frame. The plan's decision (players 60 Hz,
enemies 30 Hz, dirty-flagged) is reasonable and LAN-trivial either way — but update 00-network.md,
or the next reader re-litigates it.

### MINOR 5 — Vestigial anm step in the M1 apply pipeline

Plan §5 M1 Apply: "anims (`setSingleAnimeBase` + frame ctrls)" — but the wire format (02 §2, adopted
by the plan) carries **no anm ids** (matrix pose only). There is nothing to feed
`setSingleAnimeBase` per frame; the puppet's own calc output is overwritten anyway. The step should
read: "leave the create-time idle anm loaded; run `allAnimePlay()`/`calc()` only to keep the model
coherent, then overwrite." Harmless but confusing as written.

### MINOR 6 — Wire-format underspecifications

- 00-network §5 `PlayerState.scene u8` — TP stages are 8-char names; a u8 "scene" is undefined
  (index into what table?). The plan's room-scoping needs (stage id/hash, roomNo) spelled out.
- 02 §2.2 `PlayerEvent` "attention/lock target u16 mapped to session entity id" — undefined when the
  target is a local-only actor (NPC, sign). Needs a "none/local-only" sentinel rule.
- Enemy id key `(roomNo, setID, procName)` is per-stage ambiguous across stage changes unless the
  registry is explicitly per-stage (it will be in practice; write it down).

### MINOR 7 — Twilight semantics freeze non-twilight clients at midnight

04 §5.5 / plan M3: host in twilight publishes `daytime` forced to 0 (verified: the darkworld branch
of stock `setDaytime` pins `daytime = 0.0f` while advancing `dark_daytime`, upstream
`d_kankyo.cpp:~1595`). A client whose own darkworld flag is unset "forces synced time" → its clock
pins at 0 (midnight) for as long as the host is in twilight, and `dark_daytime` is never synced.
For the v1 co-located playstyle this is mostly moot (same stage ⇒ same twilight state), but a
story-mismatched client gets a frozen sky. Worth one sentence of acknowledged behavior.

### MINOR 8 — Offset nit in 04's weather table

04 §2.1 lists "`mThunderEff.mMode` | 0x0ED4". 0x0ED4 is the `EF_THUNDER` struct base; `mMode` is at
+0x04 (0x0ED8), `mStatus` at +0x00 (`include/d/d_kankyo.h:97-102`). Self-correcting in code (header
field access), but the table is wrong. Similarly 04's symbol table row has a typo
`__ZL14dDaKytag06_Draw…` — the real symbol is `__ZL14daKytag06_DrawP13kytag06_class` (verified in
binary; the doc's own note text has it right).

### MINOR 9 — Pervasive stock line-number drift (content correct)

Cited stock line numbers are systematically off by ±2–33 lines against current upstream (e.g.
`cc_at_check` cited :382, actual :372; `health -=` cited :475, actual :442; `mTargetedActor` cited
d_a_alink.h:4072, actual :4074; `g_env_light` extern cited d_kankyo.h:441, actual :474). Every
checked *claim of fact* was right; every *line number* should be treated as approximate. Pin a
`DUSKLIGHT_VERSION` in each doc header and stop maintaining line numbers.

### MINOR 10 — Unspecified session/state transitions

- **Save/load while connected** (mandate category d): a client loading a save mid-session triggers a
  stage change; puppet teardown is covered by the `dScnPly_Delete` bookkeeping, but session
  re-announce/WorldInit refresh vs. the client's new stage is undiscussed.
- **Client alone in a room (v1 host-authority)**: the plan never says what sims a client's room when
  the host isn't in it. Implied answer (client sims locally, unfrozen; on host arrival freeze +
  adopt host snapshots, accepting HP discontinuities) should be written down — it has real
  edge cases (enemy mid-death on transition).

---

## VERIFIED-OK (checked against source/binary; these are solid)

**Spawn/create path**
1. `fopAcM_create` public, no proc-name dedup; 9-arg signature matches the plan's call exactly
   (upstream `f_op_actor_mng.cpp:255`; binary `T __Z13fopAcM_createstjPK4cXyziPK5csXyzS1_aPFiPvE`).
2. Stock `dComIfG_play_c` is single-player sized: `mPlayerInfo[1]`, `mPlayerPtr[2]`,
   `mPlayerStatus[1][4]`, `mWindow[1]`, `mCameraInfo[1]` (upstream `include/d/d_com_inf_game.h:943-953`);
   fork diff confirms the fork grew exactly these.
3. `dStage_playerInit` single-Link guard `if (dComIfGp_getPlayer(0) != NULL || …) return 1;`
   (upstream `d_stage.cpp:1650`); `dStage_actorCreate`→`fopAcM_Create` (:1595/:1608);
   `OBJNAME("Link", fpcNm_ALINK_e)` (:535).
4. Create side effects as described: `dComIfGp_setPlayer(0,this)`/`setLinkPlayer` (:4923-24),
   `setRestartRoom` write (:5031), `setStartProcInit` (:5061), Midna+NPC tail (:5104-05),
   multi-phase with `cPhs_INIT_e` until complete, file-static `bgWaitFlg` (:4890).
5. No `numLoaded`/per-procname actor counting anywhere (`git grep` upstream: zero hits); zero
   `fopAcM_SearchByName(fpcNm_ALINK_e,…)` call sites in fork and upstream.
6. Create hijack point: no lazy init — process enters the execute queue only after create completes
   (`fpcCtRq_Do`→`fpcEx_ToExecuteQ`, upstream `f_pc_create_req.cpp:~105`; `fpcEx_Execute` requires
   `init_state == 2`, `f_pc_executor.cpp:33`; `fpcEx_ToLineQ` sets it, :55).

**sub_method freeze mechanism**
7. Instance field `fopAc_ac_c::sub_method` (0x0EC) assigned from the profile **once** under
   `fpcM_IsFirstCreating` (`f_op_actor.cpp:475-480`) → a swap at the final create phase is never
   overwritten. All four dispatches go through it (:264 draw, :370-371 execute, :421 is-delete,
   :443 delete). Table layout `{create, delete, execute, is_delete, draw}` confirmed
   (`f_pc_method.h`, `f_pc_leaf.h`, `f_op_actor.h:11-15`); `l_daAlink_Method` =
   `{daAlink_Create, daAlink_Delete, daAlink_Execute, NULL, daAlink_Draw}` (fork
   `d_a_alink.cpp:20552-20556`, all five file-local statics present in the binary's symtab).
   NULL method → returns 1 (`f_pc_method.cpp` `fpcMtd_Method`).
8. A skipped `fopAc_Execute` returning default 0 is harmless: the executor aggregates return values
   without acting on them (`cTrIt_Method`, `src/SSystem/SComponent/c_tag_iter.cpp` /
   `c_tree_iter.cpp`; `fpcBs_Execute`, `src/f_pc/f_pc_base.cpp:59`).
9. `fopAc_Execute` is a file-local static (`static int fopAc_Execute(void*)`, fork
   `f_op_actor.cpp:292`; binary `t __ZL13fopAc_ExecutePv`), gates exactly as 03 §1.2 describes,
   and `actor->old = actor->current` runs inside the non-skip branch (:357) — so the plan's
   "copy old = current after apply" is required and correct. Draw is a separate dispatch →
   "freeze AI, keep draw" via pre-hook skip works.

**Pose surface (the load-bearing one)**
10. `J3DModel::getBaseTRMtx/setBaseTRMtx/getAnmMtx/setAnmMtx/getMtxBuffer` all exist with the
    claimed semantics (upstream `J3DModel.h:93-116`); `J3DMtxBuffer::getAnmMtx` returns
    `mpAnmMtx[idx]`, `setAnmMtx` copies in, `getScaleFlag/setScaleFlag` exist
    (`J3DMtxBuffer.h`).
11. **Matrix-copy actually renders**: `calc()` computes only anmMtx/skin (`J3DModel.cpp:426-461`);
    `calcDrawMtx` runs at draw time in `viewCalc()` from the *live* anmMtx + current view
    (`J3DModel.cpp:513-544`). Post-calc `setAnmMtx` overwrites are what the GPU skinning path sees.
12. **Frame-interp is compatible if — and only if — writes go through `J3DModel::setAnmMtx`**: the
    TARGET_PC out-of-line version re-records into the interp system after writing
    (`J3DModel.cpp`: `setAnmMtx` → `dusk::frame_interp::record_final_mtx(getAnmMtx(jointNo))`).
    Plan risk #11 ("Low") understates this: writing via `J3DMtxBuffer::setAnmMtx` or MTXCopy into
    `getAnmMtx` would leave stale recorded matrices and jitter/fight the interpolator. Mandate the
    `J3DModel::` API in the apply pipeline.
13. Post-`execute` is frame-final for the pose: `modelCalc(mpLinkModel)` runs inside
    `daAlink_c::execute` (fork `d_a_alink.cpp:19041`; execute spans 18192-19426).
14. `setItemMatrix(0)` re-derives sword/shield **and** face/hat base TRs from body anmMtx(4)
    (`mpLinkFaceModel->setBaseTRMtx(mpLinkModel->getAnmMtx(4))`, fork `d_a_alink.cpp:6101-6113`) —
    so the plan's apply order (pose copy → `setItemMatrix`) correctly propagates the received pose
    to attached models. Face/hat one-frame issues are covered by the ordering, not just "1-frame
    staleness" as 02 §9.3 guessed.
15. All cited `daAlink_c` members exist in the stock public header with usable access:
    `execute()` (:1811), `create()` (:1479), `modelCalc` (:1819), `setMatrix` (:1493),
    `allAnimePlay` (:1534), `setSingleAnimeBase` (:1527), `setBodyPartPos` (:1491),
    `setAttentionPos` (:1492), `setRoomInfo` (:1480), `setCollisionPos` (:1509),
    `setWolfCollisionPos` (:1506), `setItemMatrix` (:1496), `setWolfItemMatrix` (:1497),
    `changeLink` (:2892), `changeWolf` (:2891), `setDamagePoint/Normal/Land` (:1942-44),
    `posMove` (:1687); fields `mLinkAcch` (:4026), `field_0x1f20/0x1f24` (:4040-41),
    `mFaceBtpHeap`/`mFaceBckHeap` (:4055/:4057), `field_0x215c` (:4060), `mTargetedActor` (:4074),
    `mRideActorID` (:4099), `mSelectItemId` (:4145), `mRideStatus` (:4159), `mEquipItem` (:4207).
    Symbols confirmed in binary for create/execute/modelCalc/setDamagePoint/posMove.
16. `checkWolf()` is instance state (`checkNoResetFlg1(FLG1_IS_WOLF)`, `d_a_player.h:1077`,
    `FLG1_IS_WOLF = 0x2000000` :389); `mDamageTimer` (:327), `mBodyAngle` (:340), `mDemo` (:604ish)
    as cited. `checkPlayerNoDraw` at `d_a_alink_link.inc:436` ✓.

**Combat**
17. `dCcS::SetAtTgGObjInf` is virtual, public, 11-param (upstream `include/d/d_cc_s.h:34-36`;
    def at `d_cc_s.cpp:534`; both `cCcS::` and `dCcS::` symbols in binary). The hook SDK resolves
    virtual members by display name to the actual overrider (`src/dusk/mods/svc/hook.cpp:657-696`).
18. `cc_at_check` (upstream `d_cc_uty.cpp:372`): `i_enemy->health -= i_AtInfo->mAttackPower` (:442),
    death `mHitStatus = 2; health = 0` (:447-448); `at_power_check`/`cc_pl_cut_bit_get` public
    (`d_cc_uty.h:33-36`; symbols in binary).
19. **77/117** enemy/boss files use `cc_at_check` — exact recount against `src/d/actor/d_a_[eb]_*.cpp`.
20. `e_ai_class::setCcCylinder`/`setBaseMtx` public (`d_a_e_ai.h:30/46`), `Execute()` at
    `d_a_e_ai.cpp:761`, `setBaseMtx` :823; `setCcCylinder` does register with `dComIfG_Ccsp()->Set`.
    `fopEn_enemy_c::mFlags` at 0x58E (`f_op_actor.h:424`). `daAlldie_c::actionCheck`/`actionTimer`
    public members with real symbols (`__ZN10daAlldie_c11actionCheckEv` etc.).
21. Drop creation is execute-time only (evidence for MAJOR 4): `fopAcM_createItemFromEnemyID` calls
    live in enemy death actions (e.g. `d_a_e_fz.cpp:521`); both drop functions are single-overload
    public symbols. `cCcS` per-frame clearing confirmed (`c_cc_s.cpp:481-484`).
22. Enemy/player inline target accessors are unhookable inlines as claimed:
    `daPy_getLinkPlayerActorClass` (`d_a_player.h:1315`), `fopAcM_searchPlayerAngleY` etc.
    (`f_op_actor_mng.h:735-751`). "Host enemies aggro host Link only" conclusion follows.

**Time & weather (04 is the most accurate document)**
23. Every one of the 18 cited `g_env_light` offsets is **exact**: `daytime` 0x1244,
    `time_change_rate` 0x124C, `old_time` 0x1274, `mDate` 0x12BE, `wether_pat0/1` 0x12C2/3,
    `mColpatWeather` 0x12C8, `dice_wether_mode/state/pat` 0x12C9/CA/CB, `dice_wether_counter`
    0x1298, `base_raincnt` 0x129C, `dice_wether_change_time/time` 0x11D4/11D8, `raincnt` 0x0E80,
    `mSnowCount` 0x0E8C, `field_0x130a` 0x130A (upstream `include/d/d_kankyo.h`).
24. Every cited kankyo symbol verified in the binary with the exact claimed mangling:
    `_ZN18dScnKy_env_light_c10setDaytimeEv`, `…9exeKankyoEv`, `…10getDaytimeEv`,
    `__Z19dKy_instant_timechgf`, `__Z19dKy_instant_rainchgv`, `__Z16dKy_set_nexttimef`,
    `__Z13dKyw_rain_seti`, `__Z16dKyw_wether_movev`, `__Z16dKyw_wether_procv`,
    `__Z18dKy_daynight_checkv`, `__Z19dKy_darkworld_checkv`, `__Z17dKy_change_colpath`,
    `__ZL10dKy_CreatePv`, `__ZL11dKy_ExecuteP17sub_kankyo__class`, `__ZL8dKy_Draw…`,
    `__ZL14daKytag06_DrawP13kytag06_class`, data `_g_env_light` / `_g_dComIfG_gameInfo`.
25. `setDaytime` body matches 04's description: save re-read at top, the five freeze conditions,
    `daytime += time_change_rate`, 360-wrap with `mDate++` + `dKankyo_DayProc()`, audio clock +
    save write-back tail (upstream `d_kankyo.cpp:1531-1666`). `dKankyo_DayProc` is a static-weak
    clearing temp bit 91, trivially replicable (`include/d/d_kankyo_static.h`). The dice proc runs
    only from `daKytag06_Draw` when `mType == 4` (`d_a_kytag06.cpp:290-294`) → the type-4 skip
    suppression is correct. `setDaytime`/`exeKankyo`/`getDaytime` are public members →
    `DEFINE_HOOK(&dScnKy_env_light_c::setDaytime, …)` compiles.
26. `mDoAud_setHour/Minute/Weekday` are inlines (as feared for hooking) but over
    `Z2AudioMgr::getInterface()` (inline over `mAudioMgrPtr`, a real data symbol) →
    `Z2StatusMgr::setHour` inline setters — i.e. **callable** from a mod's `setDaytime`
    replacement; only hooking them would have been impossible, and nobody proposed that.

**SDK / platform**
27. Hook semantics as the plan assumes: pre-hook skip prevents `g_orig`, post-hooks still run,
    skipped return value is value-initialized (`sdk/include/mods/hook.hpp` `HookImpl::trampoline`).
    `DEFINE_HOOK_SYMBOL` supports display + mangled names; ambiguous overloads → `MOD_CONFLICT`.
28. Symbol manifest machinery is real and embedded: symgen `manifest --embed` POST_BUILD
    (`cmake/SymbolManifest.cmake`), runtime loader (`src/dusk/mods/manifest.cpp`) with build-id
    freshness check; the built app has the `__symdbh` section and a full local symtab.
29. GameService = ABI epoch (`sdk/include/mods/svc/game.h`), auto-imported for `FEATURES game`.
30. `mod_update` runs at frame start, before all actor executes (`fapGm_Execute` → `duskExecute()` →
    `ModLoader::tick()`, `src/f_ap/f_ap_game.cpp:820,838`), so drain-in-`mod_update` →
    apply-in-execute-hook ordering is coherent. Lifecycle changes apply between frames in the same
    tick — safe for all planned hook targets (none stay on-stack).
31. Misc symbols/decls all present: `fpcLy_SetCurrentLayer`, `fpcM_FastCreate`,
    `fopAcM_SearchByID` (inline + symbol pair), `fopAcM_delete` (both overloads),
    `fpcLyIt_AllJudge` + `fpcSch_JudgeForPName` (so the inline `fpcM_SearchByName` is
    reimplementable — the fork's `coop_player.cpp:344-348` pattern ports),
    `fpcM_ERROR_PROCESS_ID_e = 0xFFFFFFFF`, `dStage_changeScene` declared public
    (`include/d/d_stage.h:1403`) with symbol, `dComIfGs_Wolf_Change_Check` symbol,
    `dSv_restart_c::setRoom` symbol (the MAJOR-1 fix), `dComIfG_get_timelayer` symbol,
    `dScnPly_Delete`/`dScnRoom_Delete` file-local statics (upstream `d_s_play.cpp:836`,
    `d_s_room.cpp:325`; binary `t __ZL14dScnPly_DeleteP9dScnPly_c` etc.), fork's spawn/layer dance
    and poll pattern as cited (`src/dusk/coop/coop_player.cpp:338-376`).

---

## TOP 5 MUST-FIX before any code is written (M0)

1. **Lifecycle design for spawned actors.** `mod_shutdown` must despawn all puppets (or restore
   their `sub_method` to the vanilla tables) before the library unloads — plus session teardown on
   disable. Add disable/reload-with-live-puppets to M1 acceptance. (BLOCKER 1)
2. **Hook-target audit against the real symbol set.** Replace the impossible
   `dComIfGs_setRestartRoom` hook with `dSv_restart_c::setRoom` (or field save/restore); use mangled
   names for the overloaded `fopAcM_create`/`fopAcM_fastCreate`/`fopAcM_delete`; in M0, resolve
   **every** planned hook target at init and fail fast. (MAJOR 1, MINOR 1)
3. **Serialize puppet creates** (one in flight, queue the rest) — stock's shared `static BOOL
   bgWaitFlg` and the plan's single spawn flags corrupt under concurrent creates. Make spawn
   suppression suppress-**all**-creates during the window. (MAJOR 2, MINOR 2)
4. **Decide puppet collision explicitly.** Either add per-frame Tg registration
   (`OnTgSetBit` + `dComIfG_Ccsp()->Set` + `SetMass` ×3) to `puppetExecute`, or strike every
   "keeps collision / participates in combat / enemy attacks hit the puppet" claim from 01 and the
   plan. (MAJOR 3)
5. **Rewrite the drop flow for the freeze model**: clients explicitly spawn drops on
   `EnemyEvent(died)` (drop-table id captured by the adapter before `fopAcM_delete`); "gating the
   native roll" only exists host-side. While there, specify death-presentation (anim + VFX beat
   before delete). (MAJOR 4)

(Next in line: resolve the weather ramp contradiction — MAJOR 5 — before M3; and fix the fork's
unbumped `GAME_SERVICE_MAJOR` + make the standalone-repo build mandatory — MAJOR 6 — before the M1
"loopback in the fork" dev step creates a fork-ABI `.dusk` that stock will happily load.)

---

## Scope arguments

**Keep as-is:** the non-sync of story/world/inventory is well-founded (verified: save flag writes
are everywhere, including inside `daAlink_c::create`); raw-matrix pose v1 (2.67 KB/frame) is justified
— the write path is interp-aware and LAN bandwidth is trivial; 30 Hz enemies is a fine decision —
propagate it into 00-network.md and move on.

**Add (cheap, high value):**
- A "Mod lifecycle" section (BLOCKER 1) — this is the plan's biggest hole and costs one paragraph.
- Adapter duties: drop spawn + death presentation (MAJOR 4) — belongs in `NetEnemyAdapter`.
- A written enemy-ownership transition rule for "client alone in a room" (MINOR 10).
- The M0 resolve-all-hook-targets self-check (MINOR 1) — turns every future doc line-number/name
  error into a load-time failure instead of a debugging session.
- Fork hygiene: bump `GAME_SERVICE_MAJOR` (MAJOR 6) regardless of this project.

**Remove / re-scope:**
- M1's "loopback parity test in the fork's split-screen during dev" — requires a fork-ABI mod build
  (the dangerous artifact of MAJOR 6) to exercise code paths (split-screen) the networked mod doesn't
  use. Replace with a single-process loopback in a **stock** build (spawn a puppet, feed it
  synthetic snapshots) — same value, no ABI hazard.
- Nothing else. The scope cut is the strongest part of this plan.
