# Adversarial review — networked co-op mod plan

Reviewer: `z-ai/glm-5.2` (adversarial pass, investigation only — no files modified except this one).
Scope: `docs/design/network.md`, `docs/design/mod-coop/00..04*.md`, `implementation-plan.md`, `docs/modding.md`, spot-checked against the in-repo source and the built fork binary `build/macos-default-relwithdebinfo/Dusklight.app/Contents/MacOS/Dusklight`.

Note on verification substrate: **this repo is the fork** (`dusklight-coop`, `TARGET_PC`). Several investigations cite "stock upstream" line numbers (e.g. `d_com_inf_game.h:944-952` with `mPlayerInfo[1]`). Those exact stock lines cannot be confirmed from this checkout because the fork has already grown the arrays to `MAX_PLAYERS`/`MAX_VIEWS` (see `include/d/d_com_inf_game.h:944-952` here, which shows the *forked* sizing). The ABI argument ("a mod cannot grow these arrays") is sound regardless; the specific stock line numbers are a verification gap, not an error. Symbol *names* for non-inlined functions match between fork and stock (only bodies differ under `#if TARGET_PC`), so `nm` on the fork binary is valid evidence for "does this symbol exist / is it inlined" claims.

---

## VERDICT: **with-caveats**

The core architecture is real and the hardest mechanical claims hold up under source/bin inspection: `fopAcM_create` has no procname dedup, the `sub_method` per-instance method-table swap is a genuine writable dispatch boundary, the J3D matrix pose surface (`getAnmMtx`/`setAnmMtx`/`getBaseTRMtx`) exists with the documented semantics, the generic enemy freeze via `fopAc_Execute` pre-hook keeps draw alive, and the time/weather symbols are present and hookable. The Anchor→TP mapping is largely faithful.

However, the plan is **not buildable verbatim as specified**. At least one listed M1 hook target is a header inline with no symbol (impossible to hook), the time-sync `rate` model assumes a fork-only code path that does not exist on stock, there is an unreconciled contradiction on enemy snapshot cadence between the transport design and the consolidated plan, and several real risks (mod-reload dangling `sub_method`, forced stage-change on join against an out-of-progression client save, host-leave) are unaddressed. All issues found have concrete fixes; none invalidate the approach. Fix the M0/M1 items below before any code lands.

---

## RANKED FINDINGS

### B1 — BLOCKER: `dComIfGs_setRestartRoom` is a header inline; it cannot be hooked as the plan demands

- **Claim (plan M1 hook list; 01 §6.2/§7):** "pre-hook `dComIfGs_setRestartRoom` → skip" to suppress the puppet create writing the host's restart-room save.
- **Evidence:** `include/d/d_com_inf_game.h:2511` —
  ```cpp
  inline void dComIfGs_setRestartRoom(const cXyz& i_position, s16 i_angle, s8 i_roomNo) {
      g_dComIfG_gameInfo.info.getRestart().setRoom(i_position, i_angle, i_roomNo);
  }
  ```
  `nm` on the built binary for `setRestartRoom|dComIfGs_setTime|dComIfGs_setDate|dComIfGs_setRestartRoomParam` returns **nothing** — there is no symbol. The SDK hooks by symbol (`DEFINE_HOOK_SYMBOL`) or by addressable function (`DEFINE_HOOK`); an inline compiles into every caller and has no target address. `install()` would return `MOD_UNAVAILABLE` and M1 fails its own hook smoke test.
- **Why it breaks the plan:** M1 lists this as a required hook ("Host save corruption — High" risk #3 relies on it). As written, the hook cannot be installed.
- **Fix (verified feasible):** Hook one level deeper. `dSv_restart_c::setRoom(cXyz const&, short, signed char)` is a real out-of-line export: `src/d/d_save.cpp:1490`, symbol `000000010005fc8c T dSv_restart_c::setRoom(...)`. Gate it with the same `puppetCreateInProgress` flag. Alternatively, save/restore the `dSv_restart_c` fields around the create call (mirrors the slot-0 pointer save/restore). Update the plan's hook list and 01 §6.2/§7 to name `dSv_restart_c::setRoom`, not the inline wrapper. (Also audit the other "suppress" hooks in the same list — `dComIfGs_*` are inlines; only the underlying `dSv_*` members are hookable.)

### B2 — BLOCKER (to time-sync correctness): the wolf-howl fast-forward slowdown is a fork-only (`#if TARGET_PC`) addition; the plan models it as "vanilla"

- **Claim (04 §1.2 table, §5.4; plan M3 §5.4):** "1.0 … fast-forward to dawn/dusk (wolf-howl skip; `setDaytime`'s `#if TARGET_PC` block drops it back to 0.012 on crossing 90/285)" and "Host publishes `rate=2` (plus a TimeEvent at the slowdown boundary, since the vanilla `#if TARGET_PC` slowdown is host-internal)."
- **Evidence:** `src/d/d_kankyo.cpp:1556-1561` —
  ```cpp
  #if TARGET_PC
  if (time_change_rate == 1.0f && (std::fmod(daytime - 90.0f + 360.0f, 360.0f) < ... || ...285...))
  {
      g_env_light.time_change_rate = 0.012f;
  }
  #endif
  ```
  The only reset of `time_change_rate` outside this block is at `dKy_Create` (stage load, `d_kankyo.cpp:1493` → 0.012) and the DEBUG ≥1000 markers. The rate is *set* to 1.0 at `src/d/actor/d_a_alink_wolf.inc:4211`. On **stock** there is no 90/285 slowdown — the howl fast-forwards at 1.0/tick continuously until the next stage load resets it.
- **Why it breaks the plan:** The `rate=2` + "TimeEvent at the slowdown boundary" is incoherent on stock: there is no slowdown boundary to detect or publish. The investigation mislabels a fork patch as "vanilla". Either the host's clock model is wrong (it expects a slowdown that won't happen), or the `rate` field is misleading documentation. Clients fast-forwarding at `rate=2` "until DAWN/DUSK event" would never receive that event from a stock host.
- **Fix:** Drop the "slowdown boundary" TimeEvent entirely. Since `TimeSync` carries absolute phase at 1 Hz, the client self-corrects regardless. Model `rate` as purely informational (the host's current `time_change_rate` bucket: 0 frozen / 1 normal / 2 fast) and let the client advance its replica by `rate × simTicks` between absolute syncs — no boundary events. Re-derive §5.4 against stock `setDaytime`, not the fork's `#if TARGET_PC` block. Verify whether stock wolf-howl even *has* a rate reset besides stage load (if not, document that the host publishes `rate=2` for the whole howl and the client runs flat 2× until the next absolute sync).

### M1 — MAJOR: internal contradiction — enemy snapshot cadence (every-frame vs 30 Hz)

- **Claim A (`00-network.md` §6):** "Enemy snapshots: same every-frame policy for enemies in the sim owner's room" and the bandwidth calc is explicitly "40 enemies × ~50 B ≈ … per frame at 60 Hz".
- **Claim B (`implementation-plan.md` M2; `03-enemies.md` §3.5, §8 P1):** "Enemy snapshots: 30 Hz with per-enemy dirty flags (decision: players every frame, enemies 30 Hz)".
- **Evidence:** the two source documents disagree, and the plan asserts a "decision" without reconciling the transport doc's every-frame bandwidth math. `00-network.md` §6 even argues *against* 30 Hz ("A 30 Hz rate would halve freshness and require interpolation … for no gain on LAN").
- **Why it matters:** This is the single most bandwidth-sensitive design parameter and the documents contradict each other. The plan's "decision" is reasonable (enemies are less input-critical), but it silently overrides a sibling design doc that argued the opposite. Implementers reading `00-network.md` will build the wrong thing.
- **Fix:** Amend `00-network.md` §6 (and its bandwidth table) to state the resolved cadence: players every-frame, enemies 30 Hz dirty-flagged, and recompute the bandwidth line (`40 × 50 B × 30 Hz ≈ 60 KB/s` enemies + `8 × 2.67 KB × 60 Hz ≈ 1.3 MB/s` players). Note that 30 Hz enemy snapshots applied every-frame in the `fopAc_Execute` pre-hook means re-applying stale state on intermediate frames — acceptable, but state it.

### M2 — MAJOR (missing risk): mod reload leaves puppet `sub_method` dangling → crash

- **Claim (plan §4; 01 §6.3 Option B):** puppets get `actor->sub_method = &g_puppetAlinkMethod` where `g_puppetAlinkMethod` is a mod-defined static.
- **Evidence:** `docs/modding.md` "Runtime Lifecycle": "Reload … load a *fresh copy* of your library." `fopAc_ac_c::sub_method` is a plain pointer field at `include/f_op/f_op_actor.h:282` (`0x0EC`) dispatched at `f_op_actor.cpp:264/370/421/443`. If the mod is reloaded (or disabled) while a puppet actor is alive, `actor->sub_method` still points into the **old** mod image's memory; after unload, the next `fopAc_Execute`/`fopAc_Draw`/`fopAc_Delete` dereferences freed memory and crashes.
- **Why it breaks the plan:** The plan's risk register (#1-#11) omits lifecycle interaction entirely. The mod SDK explicitly supports hot reload; the plan must be reload-safe or explicitly force a session restart on reload.
- **Fix:** In `mod_shutdown`, walk the puppet registry and restore each puppet's `sub_method` to `&l_daAlink_Method` (the stock table, `d_a_alink.cpp:20552`) — or `fopAcM_delete` every puppet first. Same for any enemy puppets whose state relied on mod-side tables (the enemy freeze uses a pre-hook, not a sub_method swap, so it's less fragile — but the registry/hook removal must race no in-flight dispatch). State explicitly whether reload mid-session is supported.

### M3 — MAJOR (missing risk): forced stage-change on join against an out-of-progression client save

- **Claim (plan M4; 00-network.md §4):** "Mid-game join: client warps to the host's stage/room via a forced stage change" using `dStage_changeScene`.
- **Evidence:** `dStage_changeScene` is a real export (`d_stage.cpp:2852`, symbol confirmed). But `dStage_playerInit` (`d_stage.cpp:1650` here) and the stage load path read the client's *own* save for restart point, start mode, story flags, and layer resolution. A client whose save hasn't unlocked the host's stage (no entrance flag, no dungeon item, wrong `dComIfG_get_timelayer()` for the time of day) will either fail the `JUT_ASSERT(1636, i != num)` start-point search, land on the wrong room layer, or trigger story gates it hasn't earned. The plan says "client boots their own save" but then forces a stage the save may not sanction.
- **Why it breaks the plan:** Join-mid-game is M4 acceptance. The plan gives no story/entrance validation for the warp target. At minimum this is a hard crash risk (the start-point assert); at worst it silently corrupts the client's progression by running a stage their save isn't ready for.
- **Fix:** Specify the join warp policy: (a) restrict host stages to ones the client's save has already unlocked (host advertises stage, client refuses/suggests a safe anchor if not ready), or (b) use a neutral spawn anchor and skip story init (the puppet-create suppression already has the shape for this — `procWaitInit` fallback). Document the `dStage_playerInit` guard interaction. Add an acceptance test for "client joins a host in a stage the client hasn't reached".

### M4 — MAJOR (missing risk): culling / draw gating for frozen enemy puppets is under-specified

- **Claim (03 §1.3 caveat 1):** hook `fopAcM_cullingCheck` → FALSE for puppets because `cullMtx` is stale.
- **Evidence:** `fopAc_Draw` (`f_op_actor.cpp:222-264`) gates draw on `(!fopAcM_CheckStatus(actor, fopAcStts_CULL_e) || !fopAcM_cullingCheck(actor))`. `cullMtx` (`f_op_actor.h:301`, `0x504`) is set inside each actor's own execute via `fopAcM_SetMtx`. With execute skipped, `cullMtx` is whatever it was at create/last-real-execute. If `fopAcStts_CULL_e` is set on the enemy (profile-level for some types), the stale `cullMtx` drives the frustum test and the puppet may pop out. The investigation mentions this but the plan's M2 hook list says only "hook `fopAcM_cullingCheck` → FALSE for puppets" without noting that `fopAcStts_CULL_e` may not be set at all when execute is skipped (in which case cullingCheck is never called and the hook is dead). Additionally, skipping `fopAc_Execute` entirely also skips `daSus_c::check` and `eventInfo.beforeProc()` — fine for puppets, but it means suspend boxes and event-draw-gating won't apply to puppets either; the plan should state this is intended.
- **Fix:** In M2, specify: (1) whether enemy puppets carry `fopAcStts_CULL_e` from their profile (check each whitelisted type's profile `status` word — e.g. `g_profile_E_AI`); (2) if yes, the `fopAcM_cullingCheck` hook is required; (3) if no, the hook is a no-op and can be dropped. Either way, set the puppet's `cullMtx` from `current.pos` in the apply path so the frustum test is correct even if called. Add it to the apply field list.

### M5 — MAJOR: stock `dComIfG_play_c` single-player sizing is asserted but unverifiable in this repo, and the mod's slot-0 restore assumes field offsets the mod must read from stock headers

- **Claim (01 §0/§2):** stock has `mPlayerInfo[1]`, `mPlayerPtr[2]`, `mPlayerStatus[1][4]`, `mWindow[1]`, `mCameraInfo[1]` at `d_com_inf_game.h:944-952` stock.
- **Evidence:** This checkout shows the **fork** sizing (`mPlayerInfo[MAX_PLAYERS]`, `mPlayerPtr[MAX_PLAYERS][2]`, `mPlayerStatus[MAX_PLAYERS][4]`, `mWindow[MAX_VIEWS]`, `mCameraInfo[MAX_VIEWS]` at `include/d/d_com_inf_game.h:944-952`). The stock `[1]`/`[2]` sizing is plausible and the ABI argument ("a mod cannot grow these") is correct, but the specific stock layout could not be confirmed from source here. Critically, the create-neutralization (B1/M1) reads and restores `mPlayerInfo[0].mpPlayer` and `mPlayerPtr[0]` by including the stock header — if the stock header's `mPlayerPtr` is `[2]` (Link/Horse) rather than `[MAX_PLAYERS][2]`, the mod's `mPlayerPtr[0][0]` indexing still works (index 0 of a `[2]` is the Link ptr). So the restore is likely fine, but **the mod must be built against the stock SDK headers pinned to `DUSKLIGHT_VERSION`** (plan §2 says this) — verify the pinned stock headers actually expose `mPlayerInfo`/`mPlayerPtr` as direct fields (they're under a struct with public-ish access in the fork; confirm stock).
- **Fix:** Before M1, fetch the pinned stock SDK at the `DUSKLIGHT_VERSION` commit and confirm: (a) `dComIfG_play_c` field layout and the `getPlayer`/`getPlayerPtr` inline accessors compile from stock headers; (b) `mPlayerInfo[0].mpPlayer` is writable from a mod (it's a public field in the fork; confirm stock). Add a note that the create-restore reads/writes raw struct fields and is ABI-version-locked.

### m1 — MINOR: `sub_method` line citation is the profile field, not the actor field

- **Claim (01 §5.2):** "every actor carries `sub_method` (`f_op_actor.h:18` …)."
- **Evidence:** `f_op_actor.h:18` is `actor_process_profile_definition::sub_method` (the **profile** struct at offset 0x24). The **actor instance** field is `fopAc_ac_c::sub_method` at `f_op_actor.h:282` (offset `0x0EC`), type `profile_method_class DUSK_CONST*`. The dispatch at `f_op_actor.cpp:264/370/421/443/480/569` reads `actor->sub_method` (the 0x0EC field). The mechanism is exactly as described; only the cited line is the wrong struct.
- **Fix:** Cite `f_op_actor.h:282` for the actor field; keep `:18` for the profile source. No functional impact (the Option B write `actor->sub_method = &g_puppetAlinkMethod` targets the correct 0x0EC field).

### m2 — MINOR: `setAnmMtx` is `#if TARGET_PC`-forked in this repo; on stock it's a one-line inline (still callable, not hookable)

- **Claim (01 §5.3, 02 §1.1):** `J3DModel::setAnmMtx(int, Mtx)` copies in; `getAnmMtx(int)` returns `MtxP`.
- **Evidence:** `libs/JSystem/include/JSystem/J3DGraphAnimator/J3DModel.h` — in the fork, `setAnmMtx` has an out-of-line `#if TARGET_PC` body and an `#else` inline `{ mMtxBuffer->setAnmMtx(jointNo, m); }`. On stock only the inline exists. `getAnmMtx` is inline returning `mMtxBuffer->getAnmMtx(jointNo)`. Both are **public inlines callable from a mod** (they compile into the mod and operate on the model's `mMtxBuffer`, which is a real field at `0x84`). The claim holds. Caveat: neither is *hookable* (no symbol) — the plan only ever *calls* them, never hooks them, so this is fine. Just don't let an implementer try to hook "setAnmMtx" later.
- **Fix:** Note in 02 §1.1 that `setAnmMtx`/`getAnmMtx` are inline-only (call-only, not hookable). No plan change needed.

### m3 — MINOR: M1 acceptance "loopback parity test in the fork's split-screen" uses machinery the mod can't ship

- **Claim (plan M1 Accept):** "loopback parity test in the fork's split-screen during dev."
- **Evidence:** The fork's multi-viewport split-screen (`mWindow[MAX_VIEWS]`, `coop_render.cpp`) is explicitly listed in 01 §9 as **impossible from a mod** (ABI struct growth + render pipeline edits). So the parity test harness depends on the fork, while the mod targets stock.
- **Fix:** This is fine *as a dev-only harness* (build the fork, run the mod against it, compare two views), but state that the parity test is a fork-assisted dev tool, not part of the mod's stock acceptance. The stock acceptance is "two stock builds on LAN; remote Link mirrors pose." Don't let the fork-only test become an implicit stock requirement.

### m4 — MINOR: replace-hook conflicts with other mods (coexistence)

- **Evidence:** `docs/modding.md` "Hooking Game Functions": "By default a second replace-hook on the same function is a conflict." The plan uses **replace** hooks on `daAlink_c::create`, `daAlink_c::modelCalc`, `daAlink_c::setDamagePoint` (+Normal/Land), and `dScnKy_env_light_c::setDaytime`. Any other mod replace-hooking these (a common target for gameplay tweaks) will fail to load or conflict-warn.
- **Fix:** Acceptable for v1 (single co-op mod assumption), but document it. Where possible, prefer pre/post hooks over replace (e.g. `setDamagePoint` could be a pre-hook returning `HOOK_SKIP_ORIGINAL` for puppets rather than a replace). `create` and `modelCalc` genuinely need replace (they reimplement around `g_orig`); keep those.

### m5 — MINOR: `dKy_event_proc` confirmed inlined — but the plan relies on suppressing `daKytag06_Draw` type 4, which kills *all* type-4 behavior, not just dice weather

- **Evidence:** `nm` shows `dKy_event_proc` has **no** text symbol — only its static tables (`__ZZL14dKy_event_procvE12S_time_table` etc., `s` local data). This *confirms* 04's claim it's inlined. 04 §3/§5.7 says to suppress the local dice machine by hooking `daKytag06_Draw` (real local symbol `__ZL14daKytag06_DrawP13kytag06_class`, confirmed) and skipping when `mType == 4`. But `daKytag06_Draw` for type 4 may do more than dice weather (the type dispatch in `d_a_kytag06.cpp:290` calls `dKy_event_proc` at line 259 inside the type-4 path; verify nothing else in that path is visually important). 
- **Fix:** Read `daKytag06_Draw`'s type-4 branch fully before suppressing it wholesale; if it also drives non-weather effects, suppress only the `raincnt`/`mColpatWeather` writes, not the whole draw. Minor, but it's the kind of "skip the whole function" overreach that 03 §1.3 correctly warns about for actors and 04 repeats for kankyo.

---

## VERIFIED-OK (claims checked and confirmed correct)

- **`fopAcM_create` has no procname dedup.** `fopAcM_create` (`f_op_actor_mng.cpp:271`) → `fpcM_Create` → request pipeline; no single-instance gate. The only "one Link per stage" guard is in `dStage_playerInit` (`d_stage.cpp:1650`: `if (dComIfGp_getPlayer(0) != NULL || ...) return 1;`), which a direct `fopAcM_create(ALINK,…)` bypasses. (01 §1.4 ✓)
- **`sub_method` per-instance method-table swap is real and writable.** `fopAc_ac_c::sub_method` at `f_op_actor.h:282` (`0x0EC`), assigned from profile at `f_op_actor.cpp:480`, dispatched at `:264` (draw), `:370` (execute), `:421` (is-delete), `:443` (delete), `:569` (create). `DUSK_CONST` is on the pointee, not the field — the pointer itself is mutable heap memory. (01 §5.2 mechanism ✓; citation wrong per m1)
- **Create-complete → execute-queue timing holds.** The create request pipeline runs `daAlink_c::create` to `cPhs_COMPLEATE_e` before `fpcEx_ToExecuteQ`. A sub_method write inside the create replace-hook on the completing phase precedes the first execute dispatch. (01 §5.2/§6.3 ✓)
- **`fopAc_Execute` is a file-local static and the generic freeze point; skipping it keeps draw.** Symbol `__ZL13fopAc_ExecutePv` (`t`, confirmed). Draw is dispatched separately via `fopAc_Draw` (`f_op_actor.cpp:222`). A pre-hook returning `HOOK_SKIP_ORIGINAL` skips the whole body including the `fopAcCnd_NOEXEC_e` setter, so draw is not engine-gated by the skip. (03 §1.2/§1.3 ✓; culling caveat per M4)
- **`dCcS::SetAtTgGObjInf` is a real virtual override.** Declared `include/d/d_cc_s.h:34`, defined `d_cc_s.cpp:534`; both `cCcS::SetAtTgGObjInf` and `dCcS::SetAtTgGObjInf` are exported symbols. Hookable by display name `dCcS::SetAtTgGObjInf` (unambiguous across classes). (03 §4.1 ✓)
- **J3D pose surface.** `J3DModel::getBaseTRMtx`/`setBaseTRMtx` (`J3DModel.h`, inline), `getAnmMtx(int)` → `MtxP` (inline), `setAnmMtx(int, Mtx)` (inline), `getMtxBuffer()`/`setScaleFlag(int,u8)` (inline) — all public, callable from a mod. `mMtxBuffer` field at `0x84`. (01 §5.3, 02 §1.1 ✓; call-only per m2)
- **`fopAc_ac_c` enemy fields.** `field_0x560` (s16 max HP) at `0x560`, `health` (s16) at `0x562` (`f_op_actor.h:314-315`); `fopEn_enemy_c : public fopAc_ac_c` at `:340` with `mFlags` (Dead/Down/CutDownHit/WolfBite/HeadLock…). (03 §3.2 ✓)
- **`dKy_daynight_check` boundaries.** `d_kankyo.cpp:1747` — hour ∈ [6,19) → day; 90 (6×15) dawn, 285 (19×15) dusk. Symbol `dKy_daynight_check()` exported. (04 §1.1 ✓)
- **`dScnKy_env_light_c::setDaytime` replaceable.** `d_kankyo.cpp:1531`, symbol `__ZN18dScnKy_env_light_c10setDaytimeEv` (`T`, exported). `exeKankyo` (`:4733`), `dKy_Create` (`:8297`, local `__ZL10dKy_CreatePv`), `daKytag06_Draw` (local `__ZL14daKytag06_DrawP13kytag06_class`), `g_env_light` (`_g_env_light`, `S` data) — all confirmed. (04 §3 table ✓)
- **`dKy_event_proc` is inlined away** (no text symbol; only its static tables exist) — confirms 04's "replicate, don't hook" note. (04 §2.2 ✓)
- **Zero `SearchByName(fpcNm_ALINK_e)` call sites in `src/`.** `grep` returned nothing — confirms 01 §2.1's "no category scan finds the puppet." (01 §2.1 ✓)
- **`dComIfGp_getPlayer`/`getLinkPlayer` are header inlines** (`d_com_inf_game.h:603/611`) — unhookable, compile into every caller; confirms 01 §2/03 §9 "enemy AI retargeting impractical." (01 §2.1, 03 §9 ✓)
- **Player-puppet drive members are public.** `setBodyPartPos`, `setAttentionPos`, `setMatrix`, `setItemMatrix`, `setWolfItemMatrix`, `setWolfCollisionPos`, `setCollisionPos`, `setSingleAnimeBase`, `allAnimePlay`, `setUpperAnimeParam`, `changeWolf`, `changeLink` — all declared in `include/d/actor/d_a_alink.h`. `mLinkAcch` at `0x1970`. (01 §6.4, 02 §4 ✓)
- **`daAlink_c::create/execute/modelCalc/setDamagePoint/posMove/draw` are non-virtual members** with real symbols (`daAlink_c::create`, `execute`, `modelCalc(J3DModel*)`, `setDamagePoint(int,int,int,int)`, `posMove()`, `draw()` all in `nm`). Hookable via `DEFINE_HOOK(&daAlink_c::..., ...)`; no virtual-dispatch complication. Statics `daAlink_Create/Execute/Draw` and file-local `daAlink_tgHitCallback`/`daAlink_coHitCallback` also confirmed as symbols. (01 §7 ✓)
- **`dStage_changeScene`, `fopAcM_createItemFromEnemyID`, `fopAcM_createItemFromTable`, `daAlldie_c::actionCheck`, `daAlldie_c::actionTimer`, `dKyw_rain_set`, `dKy_change_colpat`, `dKy_instant_timechg`, `dKy_set_nexttime`, `dKyw_wether_init`, `dKyw_wether_move`** — all confirmed exported symbols. (M2 drops, M3 weather, M4 warp ✓)
- **Collider flag-injection setters are public.** `dCcD_GObjInf::OnRPrm(u32)` (`d_cc_d.h:151`), `SetHitPos`, `SetHitApid`, `SetHitCallback` — public inlines. (03 §4.4 ✓)
- **`dSv_restart_c::setRoom` is a real export** (`d_save.cpp:1490`) — the fix for B1. (`nm` ✓)

---

## TOP 5 MUST-FIX before any code is written (M0)

1. **B1 — Replace the `dComIfGs_setRestartRoom` hook with `dSv_restart_c::setRoom`.** Audit every other `dComIfGs_*` "suppress" hook in 01 §6.2/§7 and the M1 list; resolve each to its underlying out-of-line `dSv_*` member (or a save/restore pattern). The M1 hook list must contain only hookable symbols, or M0's hook smoke test will fail on install.
2. **B2 — Re-derive the time-sync `rate` model against stock `setDaytime`.** Remove the "slowdown boundary" TimeEvent; document that stock wolf-howl has no 90/285 slowdown (it's a fork `#if TARGET_PC` patch) and that `rate` is informational with absolute-phase self-correction at 1 Hz. Rewrite 04 §5.4 and plan M3 accordingly.
3. **M1 — Reconcile enemy snapshot cadence across all docs.** Pick one (30 Hz dirty-flagged is fine), amend `00-network.md` §6 and its bandwidth math to match, and state the re-apply-stale-on-intermediate-frames behavior.
4. **M2 — Add mod-reload/reload-safety to the risk register and `mod_shutdown`.** Specify the `sub_method` restore (or puppet-delete-on-shutdown) and whether reload mid-session is supported. This is a crash-on-reload blocker otherwise.
5. **M3 — Specify the join-warp story/entrance policy.** At minimum: refuse/pin a safe anchor if the client's save hasn't unlocked the host's stage; document the `dStage_playerInit` start-point assert and `dComIfG_get_timelayer()` interaction. Add a "client joins un-reached stage" acceptance test.

---

## Arguments for scope additions / removals

- **Add (cheap, prevents a class of bugs):** an ABI/stock-header verification task in M0 — fetch the pinned `DUSKLIGHT_VERSION` stock SDK and confirm the struct fields the mod reads/writes (`mPlayerInfo[0].mpPlayer`, `mPlayerPtr`, `g_env_light` offsets, `fopAc_ac_c::sub_method` at `0x0EC`) compile and are at the documented offsets. The plan asserts stock layout it can't verify from the fork. One hour, de-risks M1/M5.
- **Add:** a "what happens when the host leaves" beyond "session ends." The plan (M4) and `00-network.md` §12 say no migration, but don't specify the client UX (freeze puppets + toast? return to single-player? main menu?). One paragraph.
- **Remove (defer):** the `scaleFlags` field in `PlayerState` (02 §2). It's 5 bytes/frame driving `setScaleFlag` on the puppet — verify during M1 that ignoring scale flags produces visible artifacts before shipping them. Likely droppable, saving bandwidth and a per-joint call.
- **Remove (defer):** boss union-existence bit + per-player death-flag grant (M2 phase 3 / M4). It's policy on top of plumbing and adds a room-entry round-trip. Ship "boss exists iff owner's story needs it" (the 04/03 documented fallback v1) first; add the union bit only when a playtest shows a cleared player wants to help.
- **Keep as-is (despite temptation to add):** no world-state sync. The "enemy-caused world changes leak anyway" caveat (`network.md` §5) is correctly scoped to the enemy channel; do not let it metastasize into a general world-replication layer.
