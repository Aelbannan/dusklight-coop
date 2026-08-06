# Adversarial review — M2.5 (enemy authority fix pass)

Reviewer: `z-ai/glm-5.2` (adversarial reviewer, M2.5 fix pass on M2 enemy authority).
Branch: `net-coop`. Review range: `008ff08ae8..HEAD` — five commits:
`7a4812d18b` (route E_YC/E_AI targeting reads through context — MAJOR-1),
`2a34320d68` (per-entry cullMtx — MAJOR-2), `f0169688c0` (cc_at_check slot-0
comment — MINOR-3), `44f17914c4` (byte-identical non-PC execute path — MINOR-4),
`149b4bc896` (M2.5 docs complete).
Aurora submodule at upstream `6c4c27f9` — unchanged in the range, not a regression.

Method: read the M2 findings (`review-m2-glm-5.2.md`) and the updated
`m2-design-notes.md`; diffed the M2.5 range; verified the routed accessor
mechanism (`include/f_op/f_op_actor_mng.h`, `include/d/actor/d_a_player.h`,
`src/dusk/coop/coop_context.cpp`, `coop_enemy.cpp`); traced each of the 9
routed sites in `d_a_e_yc.cpp` / `d_a_e_ai.cpp` for preprocessor balance, type
correctness, and actual use of the routed variable; verified the registry
container type for pointer-stability; forced a rebuild of every touched TU +
full `dusklight` link; ran `dusk_net_selftest`. No source modified; only this
review written.

---

## VERDICT

**M2.5 resolves both MAJORs and both MINORs. HEAD builds clean and is
regression-clean. Accept — no must-fix before M3.**

| M2 finding | Status | Evidence |
|---|---|---|
| MAJOR-1 (D3 targeting gap, E_YC 6 + E_AI 3 sites) | ✅ FIXED | All 9 `dComIfGp_getPlayer(0)` reads in `d_a_e_yc.cpp` / `d_a_e_ai.cpp` now sit in `#else` vanilla branches; the `#if TARGET_PC` branch uses `fopAcM_getContextPlayer()` / `daPy_getPlayerActorClass()` (both → `currentTargetPlayer()`, slot-0 fallback). Preprocessor balanced (6/6/6 in E_YC, 3/3/3 in E_AI). Types preserved per site. |
| MAJOR-2 (cullMtx shared buffer) | ✅ FIXED | `EnemyEntry` now owns `Mtx cullMtx{}`; `puppetExecute` writes `e->cullMtx` and `fopAcM_SetMtx(actor, e->cullMtx)`. Registry is `std::unordered_map<u16, EnemyEntry>` (node-based) → `&e->cullMtx` stable across rehashes/inserts. No shared/static/thread_local buffer remains. |
| MINOR-3 (cc_at_check slot-0 doc) | ✅ FIXED | 6-line comment added at `d_cc_uty.cpp:371` (above the slot-0 read) referencing `m2-design-notes §5`. Code unchanged — behavior identical. |
| MINOR-4 (non-PC byte-identical execute) | ✅ FIXED | `f_op_actor.cpp` now `#if TARGET_PC`-guards the whole lambda; `#else` is the original one-liner `ret = fpcMtd_Execute((process_method_class DUSK_CONST*)actor->sub_method, actor);` — verbatim match to pre-M2 vanilla (`bb235ed585~1`, line 338). |
| Build | ✅ | Forced rebuild of all 5 touched TUs + full `dusklight` link clean (only pre-existing JSystem `std::iterator`/`offsetof` deprecation warnings; no new warnings). |
| Selftest | ✅ | `dusk_net_selftest` `PASS: all checks succeeded` incl. all `RunM2RelayPolicyCheck` assertions. |

---

## RANKED FINDINGS

### BLOCKER
None.

### MAJOR
None. Both M2 MAJORs are resolved with correct, non-leaky mechanisms (verified
below). No new MAJORs introduced by the fix pass.

### MINOR-1 — E_AI `damage_check` routed `player` variable is dead code (vanilla-faithful, redundant routing)

`src/d/actor/d_a_e_ai.cpp:197` — the M2.5 fix routed the `player` declaration
in `damage_check()` through `fopAcM_getContextPlayer()`. The variable is
**never read** in the function body (verified: `awk '/^void
e_ai_class::damage_check/,/^}/'` finds `player` only at the declaration in both
the `#if` and `#else` branches). This is not a M2.5 regression — the same dead
`fopAc_ac_c* player = dComIfGp_getPlayer(0);` exists in vanilla at the
pre-M2.5 line 183 (`git show 008ff08ae8:src/d/actor/d_a_e_ai.cpp`). The
**meaningful** player read in `damage_check` is the
`daPy_getPlayerActorClass()->getCutType()` at line 255, which was *already*
routed through the context via the inline accessor in M2 — so E_AI
`damage_check`'s D3 coverage was already correct in M2 via line 255, and the
M2.5 routing at line 197 is redundant (it routes a read the compiler elides at
`-O2` since `dComIfGp_getPlayer` is a pure pointer load with no side effects).

Consequence: the M2 review's "E_AI damage_check reads slot-0 at :183" was a
slight miscount — that read is dead. M2.5 routed it anyway, so coverage is
≥ complete (not a gap). Net behavior on TARGET_PC is identical to vanilla
(both elide the dead load).

**Fix (optional, cosmetic):** drop the unused `player` declaration in
`damage_check` on both branches, or leave it as vanilla-faithful dead code.
Not blocking; not a regression. Listed only so the next pass doesn't re-flag
it.

### MINOR-2 — `m2-design-notes.md §5` still says "per-type verified" without cross-ref to §2.1

§2.1 ("M2.5 correction — targeting-read coverage (D3) was overstated") now
honestly documents that the original "per-type verified" covered only the
combat/collider surface, not targeting reads, and that M2.5 routed E_YC 6/6
and E_AI 3/3. This corrects the overclaim. However, §5 "Retry resolution (M2
landed)" still closes with "**Whitelist v1 (final, per-type verified):** `E_AI`,
`E_HM`, `E_DF`, `E_YC`, `E_MD`, `B_TN`." A reader landing directly at §5 (the
final-resolution summary) could still read "per-type verified" as
all-encompassing without scrolling up to §2.1.

**Fix (optional, doc):** add a parenthetical to §5's line, e.g. "per-type
verified (combat/collider surface; targeting-read coverage corrected in
§2.1)". Not blocking.

### MINOR-3 (new observation, not an M2 finding) — `fopAcM_toActorShapeAngleY` unused-parameter asymmetry

Unrelated to M2.5 and pre-existing in M2: `include/f_op/f_op_actor_mng.h:768`
`fopAcM_toActorShapeAngleY(i_actorA, i_actorB)` takes two actors while the
`*Player*` wrapper passes `fopAcM_getContextPlayer()`. Not touched by M2.5;
noted only for completeness. No action.

---

## VERIFIED-OK

1. **MAJOR-1 / D3 routing — preprocessor structure compiles.** Per-TU
   directive counts: `d_a_e_yc.cpp` `#if TARGET_PC`=6 / `#else`=6 / `#endif`=6;
   `d_a_e_ai.cpp` 3/3/3. Every routed site declares `player` (or reads inline)
   once per branch — no duplicate declarations, no scoping errors, no nested
   `#if` overlap. The build (below) confirms compilation.

2. **MAJOR-1 / D3 routing — accessor resolves the scoped context.**
   `currentTargetPlayer()` (`coop_context.cpp:25`) returns `g_stack.back()` when
   the thread-local stack is non-empty, else `dComIfGp_getPlayer(0)`.
   `ScopedEnemyTarget::ScopedEnemyTarget` (`coop_context.cpp:65`) pushes
   `resolveNearestPlayer(enemy->current.pos)`, which returns slot-0 when
   `!sessionActive()`, else the nearest of {host Link, remote puppets} by XZ
   distance. RAII pop in the destructor — no double-push/leak. Outside any
   scope (camera/NPC/other consumers) the fallback is slot-0 → vanilla.

3. **MAJOR-1 / D3 routing — covered sites are the actual targeting paths.**
   - **E_YC (6/6):** `damage_check` (mTargetPos/wolf-bite cut-type gating),
     `e_yc_f_fly` (`mTargetPos.y = player->current.pos.y + 1000`),
     `e_yc_hovering`, `e_yc_attack` (`mTargetPos = player->current.pos` — the
     chase target), `e_yc_wolfbite` (the bite target + `shape_angle.y + 0x8000`
     facing), `daE_YC_Execute` (`S_area_dis` — the action-state distance gate).
     Plus `action()` already routed via `fopAcM_searchPlayerAngleY/DistanceXZ`
     (lines 588-589, routed inlines). E_YC now flies toward and bites the
     nearest real player.
   - **E_AI (3/3, +2 via inlines):** `player_way_check` (facing gate),
     `pl_check` (passes `player` to `other_bg_check(player)` — the attack LOS
     gate), `damage_check` (routed but dead — see MINOR-1; the real read at
     line 255 `daPy_getPlayerActorClass()->getCutType()` was already routed in
     M2). `action()` routes `m_angleToPlayer`/`m_playerDist` via
     `fopAcM_searchPlayer*` (lines 683-684). E_AI now turns toward AND attacks
     the nearest real player.
   - **E_HM / E_DF / E_MD / B_TN:** grep confirms zero direct
     `dComIfGp_getPlayer(0)` reads — their player reads go through the
     context-routed inlines, already covered in M2.

4. **MAJOR-1 / D3 routing — type correctness per site (no cast defeats).**
   Sites that originally cast to `daPy_py_c*` (`damage_check`, `e_yc_wolfbite`
   — both use `daPy_py_c`-specific members `getCutType`,
   `onWolfEnemyBiteAll`, `offWolfEnemyHangBite`, `checkWolfEnemyBiteAllOwn`,
   `eyePos`) use `daPy_getPlayerActorClass()` → `(daPy_py_c*)
   currentTargetPlayer()`. Sites that used `fopAc_ac_c*` use
   `fopAcM_getContextPlayer()` → `fopAc_ac_c*`. No site swaps a `daPy_py_c*`
   need to a `fopAc_ac_c*` accessor (which would force a cast and lose the
   typed methods). Remote puppets are `daAlink_c` (derives `daPy_py_c`), so
   wolf-bite hang mechanics operate on the remote player's replicated actor —
   the D3 goal realized, not a type trick.

5. **MAJOR-1 / D3 routing — zero UNROUTED direct reads remain on PC.** grep
   finds exactly 9 `dComIfGp_getPlayer(0)` occurrences across E_YC (6) + E_AI
   (3); every one is inside a `#else` branch (never compiled on TARGET_PC).
   The only `dComIfGp_getPlayer(0)` calls that actually execute on PC are inside
   `currentTargetPlayer()`/`resolveNearestPlayer` (the fallback) and the
   non-coop code paths — never a direct enemy-AI read. E_HM/E_DF/E_MD/B_TN have
   none.

6. **MAJOR-1 / scope push gating.** `hostNeedsContext` (`coop_enemy.cpp:947`)
   returns false unless `SessionLive() && IsHost() && actor != nullptr &&
   isRegistered(actor) && AdapterForActor(actor) != nullptr`.
   `AdapterForActor` keys on `fopAcM_GetName` → only whitelisted types have an
   adapter; `isRegistered` requires the actor be in the enemy registry. So the
   `ScopedEnemyTarget` is pushed ONLY around the host's whitelisted-enemy
   execute, never on clients, never for non-enemies, never with no session.

7. **MAJOR-2 / per-entry cullMtx — stable address.** Registry is
   `std::unordered_map<u16, EnemyEntry> g_entries` (`coop_enemy.cpp:316`) — a
   node-based container; element addresses are stable across rehash and across
   insert/erase of other entries (unlike a `std::vector`, which would
   invalidate `&entry->cullMtx` on reallocation). `EnemyEntry` is
   move-only (`delete` copy, `default` move) but `unordered_map` never moves
   existing nodes on insert. `Mtx` is `f32[3][4]` (`dolphin/mtx.h:22`); `MtxP`
   is the pointer; `e->cullMtx` decays to `MtxP` when passed to
   `fopAcM_SetMtx(actor, MtxP)` → `actor->cullMtx = e->cullMtx`. Per-frame
   `std::memcpy(e->cullMtx, mDoMtx_stack_c::get(), sizeof(Mtx))` rewrites the
   same stable buffer. Each frozen puppet now culls on its own position; the
   "last-puppet-wins" bug is gone. No shared/static/thread_local `Mtx` remains
   in `puppetExecute`.

8. **MINOR-3 — comment-only, no behavior change.** `git diff` for
   `d_cc_uty.cpp` adds only the 6-line comment above
   `daPy_py_c* player_p = (daPy_py_c*)dComIfGp_getPlayer(0);`; the executable
   line is byte-identical to pre-M2.5. The limitation (owner's equipment
   multipliers applied, not the remote attacker's) is now documented at the
   site as the M2 review required.

9. **MINOR-4 — non-PC path byte-identical to vanilla.** `f_op_actor.cpp` M2.5
   `#else` is `ret = fpcMtd_Execute((process_method_class DUSK_CONST*)
   actor->sub_method, actor);` — verbatim match to pre-M2 vanilla at
   `bb235ed585~1:src/f_op/f_op_actor.cpp:338`. The lambda is now wholly under
   `#if TARGET_PC`. (Note: in this fork `TARGET_PC=1` is always in
   `GAME_COMPILE_DEFS` for the `dusklight` target, so the `#else` is not
   compiled here — it is a static-reconstruction guarantee for a hypothetical
   vanilla build. The runtime net-disabled path runs the `#if TARGET_PC`
   branches, which fall back to slot-0 via `currentTargetPlayer()` /
   `resolveNearestPlayer` when `!sessionActive()` → behaviorally vanilla.)

10. **Build & regression.** Forced rebuild of `d_a_e_yc.cpp`, `d_a_e_ai.cpp`,
    `f_op_actor.cpp`, `coop_enemy.cpp`, `d_cc_uty.cpp` + full `dusklight` link:
    clean. Only pre-existing warnings — JSystem `std::iterator` deprecation
    (`search.h:74`) and `offsetof` on non-standard-layout (`stb.h:126`,
    `ctb.h:27`, `jstudio-control.h:32`); the pre-existing `ld: ignoring
    duplicate libraries` note. **No new warnings, no errors.** `dusklight`
    links with 33903 exports / 2053 objects.

11. **Selftest.** `dusk_net_selftest` → `PASS: all checks succeeded`,
    including the full `m2: enemy/combat relay policy (star seam)` block (host
    starts, attacker/observer join, CombatIntent NOT relayed, EnemySnapshot
    owner→clients only, CombatResult/EnemyEvent simulcast, A's
    EnemySnapshot/CombatResult NOT relayed to B).

12. **Conventions / scope.** All attachments are additive `#if TARGET_PC` with
    vanilla `#else`; guard hygiene clean (no `#ifdef`/`#if defined` mix, no
    stray `#endif` without `#if`). No M3 (time/weather) or M4 (session-polish)
    creep — the M2.5 diff touches only enemy-targeting TUs, cullMtx, the
    execute guard, the cc_at_check comment, and the design notes. Commit
    hygiene: 5 logical commits (MAJOR-1 → MAJOR-2 → MINOR-3 → MINOR-4 → docs),
    each self-contained, no fixup/squash noise. No dead code introduced by the
    fixes (MINOR-1's dead `player` is vanilla-faithful, not introduced here).

13. **Docs honesty.** `m2-design-notes.md §2.1` now explicitly corrects the
    targeting-read overclaim: it lists the E_YC 6 sites and E_AI 3 sites by
    name, notes E_HM/E_DF/E_MD/B_TN had no direct reads, and states the
    post-M2.5 coverage (E_YC 6/6, E_AI 3/3). §2.2 documents the cullMtx fix.
    The coverage description matches the code exactly. (Minor nit MINOR-2: §5
    still lacks a cross-ref.)

---

## MUST-FIX before M3

**None.** Both MAJORs and both MINORs are resolved; HEAD builds clean and
selftests PASS. The two MINORs above (dead routed `player` in E_AI
`damage_check`; §5 cross-ref) are cosmetic/doc-only and can ride or be
trivially cleaned up at M3's convenience.
