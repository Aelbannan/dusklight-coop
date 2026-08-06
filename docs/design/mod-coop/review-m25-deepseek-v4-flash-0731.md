# Adversarial review — M2.5 (enemy authority fix pass)

Reviewer: `deepseek/deepseek-v4-flash-0731` (adversarial reviewer, M2.5 fix pass on
M2 enemy authority). Branch: `net-coop`. Review range: `008ff08ae8..HEAD` — five
commits: `7a4812d18b` (route E_YC/E_AI targeting reads through context — MAJOR-1),
`2a34320d68` (per-entry cullMtx — MAJOR-2), `f0169688c0` (cc_at_check slot-0
comment — MINOR-3), `44f17914c4` (byte-identical non-PC execute path — MINOR-4),
`149b4bc896` (M2.5 docs complete).
Aurora submodule at upstream `6c4c27f9` — unchanged in the range (`git submodule
status`), not a regression.

Method: read the M2 findings (`review-m2-glm-5.2.md`) and the updated
`m2-design-notes.md`; diffed the M2.5 range; traced the context mechanism
(`include/f_op/f_op_actor_mng.h`, `include/d/actor/d_a_player.h`,
`src/dusk/coop/coop_context.cpp`); checked every `dComIfGp_getPlayer(0)` hit in
`d_a_e_yc.cpp`/`d_a_e_ai.cpp` against its surrounding `#if TARGET_PC`/`#else`
structure (not raw grep counts); confirmed each routed variable is genuinely used
in the targeting path; verified the registry container type and erase timing for
cullMtx pointer stability; compared the non-PC execute line against the vanilla
file (`eed14acdc6`); forced a rebuild of all five touched TUs + the full
`dusklight` link; ran `dusk_net_selftest`. No source modified; only this review
written.

---

## VERDICT

**M2.5 resolves both MAJORs and both MINORs. HEAD builds clean, links clean, and
is regression-clean. Accept — no must-fix before M3.**

| M2 finding | Status | Evidence |
|---|---|---|
| MAJOR-1 (D3 targeting gap, E_YC 6 + E_AI 3 direct slot-0 reads) | ✅ FIXED | All 9 reads now sit only in `#else` vanilla branches; the `#if TARGET_PC` branch uses `fopAcM_getContextPlayer()` / `daPy_getPlayerActorClass()` (both → `currentTargetPlayer()`: stack-top when a scope is pushed, slot-0 otherwise). Preprocessor balanced per site (6/6/6 in E_YC, 3/3/3 in E_AI). Pointer types preserved per site — no routing-defeating casts. Zero unrouted direct reads in PC builds. |
| MAJOR-2 (cullMtx shared `static thread_local` buffer) | ✅ FIXED | Each `EnemyEntry` owns `Mtx cullMtx{}` (coop_enemy.cpp:290); `puppetExecute` writes it per-entry and passes `e->cullMtx` to `fopAcM_SetMtx` (:942-943), which stores the pointer (f_op_actor_mng.h:308-310). Container is `std::unordered_map` (coop_enemy.cpp:316) → element references are stable across insert/rehash; erase is coupled to actor deletion, so no dangling reads. No shared/static/thread_local cull buffer remains. |
| MINOR-3 (cc_at_check slot-0 limitation not documented in code) | ✅ FIXED | Comment added at d_cc_uty.cpp:373-378, immediately above the slot-0 read (:379) that feeds the multiplier block (:434 `getSwordAtUpTime`). Comment-only diff; no behavior change. |
| MINOR-4 (fopAc_Execute lambda not byte-identical on non-PC) | ✅ FIXED | The whole lambda is now inside `#if TARGET_PC`; the `#else` is the original one-liner, textually identical to vanilla `eed14acdc6:338` (`ret = fpcMtd_Execute((process_method_class DUSK_CONST*)actor->sub_method, actor);`). |

Build/regression: forced recompile of `d_a_e_yc.cpp`, `d_a_e_ai.cpp`,
`d_cc_uty.cpp`, `coop_enemy.cpp`, `f_op_actor.cpp` + full `dusklight` link:
clean, no warnings pointing into any touched TU (only pre-existing JSystem
`-Winvalid-offsetof`/`std::iterator` header noise and the pre-existing
duplicate-libraries link warning). `dusk_net_selftest`: **`PASS: all checks
succeeded`** (relay-policy assertions all pass). Working tree tracked-clean.

---

## RANKED FINDINGS

### BLOCKER
None.

### MAJOR
None.

### MINOR

1. **MINOR-A — E_AI `damage_check` routing is a behavioral no-op (dead variable
   in vanilla too).** src/d/actor/d_a_e_ai.cpp:197 declares
   `fopAc_ac_c* player = fopAcM_getContextPlayer();` (and :199 the `#else`
   `dComIfGp_getPlayer(0)`), but the variable is **never used** anywhere in the
   function body (verified over the full span :193-358; the only player-dependent
   statement is :255 `daPy_getPlayerActorClass()->getCutType()`, which routes
   independently). This is faithful to vanilla — the original M2-era line :183
   was equally dead (`git show 008ff08ae8:src/d/actor/d_a_e_ai.cpp`). So the
   "E_AI 3/3 sites routed" claim in m2-design-notes §2.1 includes one site with
   zero behavioral effect; the *functional* E_AI fixes are `player_way_check`
   (:140, facing) and `pl_check` (:179 → `other_bg_check(player)` at :190, the
   attack LOS gate), both of which use the routed value. Impact: none today.
   **Fix (optional):** leave as-is (keeps the "zero unrouted reads" invariant
   grep-clean and matches vanilla's dead declaration) or add a one-line comment
   noting the declaration is kept for parity. Do not delete — the `#else` must
   stay vanilla-shaped.

2. **MINOR-B (pre-existing, not introduced by M2.5) — `ScopedEnemyTarget::previous_`
   is dead state.** include/dusk/coop/coop_context.h:49 declares `previous_`,
   coop_context.cpp:69 writes it, :82 silences it (`(void)previous_;`) and never
   reads it; the destructor restores the stack by popping, so `previous_` carries
   no information. Harmless (RAII pop is correct), but it is dead code in the
   M2-era context mechanism. Out of M2.5 scope; noting for a future cleanup.

3. **MINOR-C (informational) — the slot-0 documentation now in `cc_at_check` does
   not cover `cc_pl_cut_bit_get`.** d_cc_uty.cpp:92 also reads
   `dComIfGp_getPlayer(0)` (cut-bit/parry classification, vanilla M1-era code).
   It is unrelated to the enemy-damage multiplier path MINOR-3 documents (the
   synthetic-hit path enters via `cc_at_check`/`at_power_check`, not
   `cc_pl_cut_bit_get`), so this is out of M2.5 scope — noted so the code-site
   comment is not later read as covering *all* slot-0 reads in the file.

### Fixed-but-worth-knowing notes (not findings)

- **E_YC `e_yc_f_fly`'s two player reads** (the M2 review's :192/:221) both flow
  through the single routed declaration at :200 — verified the routed `player`
  feeds `mTargetPos.y` (:229) and the hover-altitude logic; no read bypasses the
  context. Same for `e_yc_attack` (`mTargetPos = player->current.pos`, :336),
  `daE_YC_Execute` (engage-distance `dist_x/dist_z -= player->current.pos`, :652-653),
  and `e_yc_wolfbite` (`player->offWolfEnemyHangBite()` / `player->eyePos`
  hitmark, :491/:498).
- **Scope reachability:** all 9 routed sites are called only from the enemy's
  `Execute`/`action` chain, i.e. only inside the `ScopedEnemyTarget` window on
  the host (`fopAc_Execute` :364-370, guarded by `hostNeedsContext`). Clients are
  frozen (`puppetExecute` short-circuits the vanilla dispatch) and never run
  these reads; non-whitelisted actors and no-session runs never push a scope, so
  `currentTargetPlayer()` falls back to slot 0 — single-player behavior is
  untouched (net.enabled=false path compiles the `#else` reads).

---

## VERIFIED-OK

1. **MAJOR-1 (a) — preprocessor structure.** E_YC: 6 `#if TARGET_PC`/`#else`/
   `#endif` triples at :58-64, :198-203, :284-289, :327-332, :452-457, :642-647 —
   balanced, each branch declares one local, use sites after `#endif` consume the
   same variable; forced rebuild confirms no duplicate-declaration/scoping
   errors. E_AI: 3 triples at :137-143, :176-182, :195-200, same pattern.
2. **MAJOR-1 (b) — routed accessor semantics.** `fopAcM_getContextPlayer()`
   (f_op_actor_mng.h:730-737) and `daPy_getPlayerActorClass()`
   (d_a_player.h:1288-1297) both call `dusk::coop::currentTargetPlayer()`
   (coop_context.cpp:25-31), which returns `g_stack.back()` when a scope is
   pushed (nearest player via `resolveNearestPlayer` :35-58: host Link vs
   same-room, non-hidden remote puppets by `absXZ` distance) and
   `dComIfGp_getPlayer(0)` when the stack is empty. `ScopedEnemyTarget` ctor
   (:65-71) pushes `resolveNearestPlayer(enemy->current.pos)`, dtor pops — RAII,
   no leak/double-pop (verified push guard `pushed_`).
3. **MAJOR-1 (c) — actual targeting-path coverage.** E_YC: `damage_check` (cut-type
   wolf-bite guard), `e_yc_f_fly` (glide target height), `e_yc_hovering` (hover
   altitude), `e_yc_attack` (dive target `mTargetPos`), `e_yc_wolfbite` (bite
   damage + hitmark), `daE_YC_Execute` (engage-range decision) — the exact sites
   the M2 review flagged, plus the M2-routed inlines (:588-589
   `fopAcM_searchPlayerAngleY/DistanceXZ`). E_AI: `player_way_check` (facing),
   `pl_check` (LOS gate via `other_bg_check(player)` :190), `damage_check` (dead,
   see MINOR-A); `action()` :683-684 already routed via `fopAcM_searchPlayer*`.
4. **MAJOR-1 (d) — type correctness.** `dComIfGp_getPlayer` returns
   `fopAc_ac_c*` (d_com_inf_game.h:3554). E_YC `damage_check`/`e_yc_wolfbite` use
   `daPy_getPlayerActorClass()` → `daPy_py_c*` (both branches, no new cast);
   E_YC `f_fly`/`hovering`/`attack`/`Execute` and all E_AI sites use
   `fopAcM_getContextPlayer()` → `fopAc_ac_c*`. The only cast in the picture is
   the accessor's own `(daPy_py_c*)currentTargetPlayer()` — a valid downcast
   since a remote puppet is a `daAlink_c` (`daAlink_c : public daPy_py_c`,
   d_a_alink.h:231; `daPy_py_c : public fopAc_ac_c`, d_a_player.h:326). No cast
   defeats the routing.
5. **MAJOR-1 (e) — zero unrouted direct reads on PC.** Every `dComIfGp_getPlayer(0)`
   occurrence in the two TUs sits between an `#else` and its `#endif` (grep
   verified line-by-line; E_YC :63/202/288/331/456/646, E_AI :142/181/199). E_HM/
   E_DF/E_MD/B_TN have 0 occurrences — consistent with m2-design-notes §2.1.
6. **MAJOR-2 — per-entry cull matrix.** `EnemyEntry::Mtx cullMtx{}`
   (coop_enemy.cpp:290); `puppetExecute` :942-943 writes it from
   `mDoMtx_stack_c` and `fopAcM_SetMtx(actor, e->cullMtx)` (stores the pointer,
   f_op_actor_mng.h:308-310). Stability: `g_entries` is `std::unordered_map`
   (:316) — element references survive insert/rehash, so `&e->cullMtx` is stable
   across new enemy registrations; erase happens only when the actor is gone —
   client death-beat cleanup erases *after* `fopAcM_delete` (:1128-1132), host
   `PollHostDeaths` erases only when `fopAcM_SearchByID(e.pid) != e.actor`
   (:474-505) — so no draw ever reads a freed entry. The Dying-frame early return
   (:892-895) intentionally keeps the last pose + last cullMtx. Grep confirms no
   remaining `static`/`thread_local` cull buffer in `src/dusk/` (only unrelated
   OSThread TLS and the targeting `g_stack`).
7. **MINOR-3 — documented at the code site.** d_cc_uty.cpp:373-378 comment states
   the owner-equipment-multiplier v1 limitation and points at
   m2-design-notes §5; diff is comment-only (+6 lines, `f0169688c0`).
8. **MINOR-4 — byte-identical non-PC path.** f_op_actor.cpp:372-376: `#else` is the
   bare one-liner; verified textually identical to the last pre-coop vanilla file
   (`eed14acdc6:338`). `ret` is declared at :294 (unchanged, `int ret = 1;`) and
   assigned by whichever branch compiles; no stray unguarded statement between
   `#if` and `#endif` pairs; guard count balanced (the file's `#if DEBUG`
   regions also still pair correctly).
9. **Build & regression.** Forced rebuild of the 5 touched TUs → zero warnings
   attributed to those files; full `dusklight` link succeeds (33903 exports,
   only the pre-existing duplicate-libs ld warning). `dusk_net_selftest` built
   and ran: **PASS: all checks succeeded** (incl. EnemySnapshot relay,
   CombatIntent no-relay, EnemyEvent/CombatResult simulcast, owner-consumption
   assertions). Aurora untouched (`6c4c27f9`); tracked tree clean after review
   (`git status` shows no modifications).
10. **Docs — m2-design-notes §2 is now honest.** §2.1 retitles the old "per-type
    verified" claim as an explicit M2.5 correction: E_YC 6/6 and E_AI 3/3 sites
    routed, E_HM/E_DF/E_MD/B_TN had none, the remaining grep hits are vanilla
    `#else` branches "never compiled on PC," and the slot-0 fallback semantics
    outside a scope. §2.2 documents the MAJOR-2 cullMtx fix and why the shared
    buffer was wrong (pointer storage + last-writer-wins). This directly retracts
    the overclaim MAJOR-1 was built on.
11. **Conventions/scope/commit hygiene.** Five single-purpose commits, each
    touching exactly its fix's files (routing → 2 enemy TUs; cullMtx →
    coop_enemy.cpp; comment → d_cc_uty.cpp; lambda → f_op_actor.cpp; docs → design
    notes). No M3/M4 creep. No dead code left by the fixes (the old
    `s_puppetCullMtx` is fully removed — no references remain). Comments at every
    routed site follow the existing `// Co-op (M2.x, D3): ...` style.

---

## MUST-FIX before M3

**None.** Both MAJORs are genuinely fixed and the fixes are structurally sound
(preprocessor-balanced, type-correct, pointer-stable, regression-clean). The
three MINORs above are non-blocking (one is a faithful-to-vanilla no-op, one is
pre-existing M2 dead state, one is informational). M2.5 is an accept; the D3
"enemies chase both players" criterion now holds for the full v1 whitelist
(E_YC 6/6 and E_AI 3/3 direct reads routed; E_HM/E_DF/E_MD/B_TN already clean).
