# M2 design notes — combat validation & whitelist (captured from the errored M2 agent)

Status: **Interim notes.** The first M2 implementation agent errored (OpenRouter
insufficient balance) before committing code. These are its verified design
conclusions, captured so the retry starts here instead of from scratch.
Companions: `03-enemies.md` (mechanics), `implementation-plan.md` Rev 3 §5 M2.

## 1. Combat damage reproduction (the hard problem — SOLVED for v1)

**Problem:** the client computes the damage of its hit locally, but the owner
applies damage through the enemy's OWN damage handler (`cc_at_check` /
`At_Check` / `damage_check`), which recomputes power via `at_power_check()`
(deterministic: `power = f(atp, mPowerType)` tables) and — in `cc_at_check`
only — applies Link equipment multipliers by reading **slot-0** on the owner's
machine. The client's computed damage (with the *client's* equipment) is not
reproducible on the owner.

**v1 resolution (LAN, trust model):**
- The client sends the RAW attack fields (`atp`, `atType`, `powerType`,
  `hitType`) and the owner reproduces damage deterministically via the
  synthetic At collider — `at_power_get` is a pure function of
  `(atp, mPowerType)`, no attacker state.
- `at_power_check()` itself applies NO Link multipliers (verified: the
  multiplier block lives in `cc_at_check`, d_cc_uty.cpp ~434, reading slot 0).
  Inline handlers (`E_HM At_Check` = `S16_SUB(health, mAttackPower)` after
  `at_power_check`; `E_FB damage_check` → `cc_at_check`) are therefore
  deterministic — the synthetic collider with `intent.atp/powerType`
  reproduces the client's damage exactly.
- Enemies using the shared `cc_at_check` will additionally get the OWNER's
  Link multipliers (slot-0 read) — **documented v1 limitation**: remote
  players' damage vs `cc_at_check` enemies does not carry their own equipment
  multipliers. Acceptable for LAN v1; revisit with a context-scoped
  `at_power_check` later.

**Synthetic attacker collider (the crash trap):**
- The fabricated At object must be a **`dCcD_Obj` with its `dCcD_GObjInf`
  populated** — a raw `cCcD_Obj` returns NULL from `GetGObjInf()`, and
  `at_power_check` reads through `dCcD_GetGObjInf(...)` → crash. Set Atp /
  AtType / AtSe / Actor (the attacker's puppet) on the full `dCcD_Obj`.
- Tg hit flag: set on the enemy's damage collider (`SetHit`), consumed by the
  owner's next execute (`ChkTgHit()` poll), cleared via `ClrTgHit()` by the
  enemy's own handler. The frozen client never runs `damage_check`, so a flag
  set on the owner is consumed there; on clients, flags are never set (their
  damage is forwarded, not applied).

**Multi-hit batching:** per-enemy-per-frame aggregation on the owner — total
damage + strongest reaction (model on `local-coop-poc` `coop_combat.cpp`
`strongestHitForVictim`).

## 2. Whitelist per-type verification findings

- **`E_GS` (Ghost Soldier): NOT a combat whitelist candidate** — no damage
  collider, no `ChkTgHit` poll, no `cc_at_check`; it "dies" via `health = 100`
  at create (wolf-power ghost). The whitelist needs per-type verification, not
  name-based inclusion. **Drop `E_GS` from the v1 whitelist.**
- **`E_AI` (Armos): hit-count semantics** — `health` reset to 1000 every frame,
  dies on `m_hitCount >= 3` (or specific AtTypes). `semantics = HitCount`.
- **`E_HM` (Torch Slug): deterministic inline handler** — `S16_SUB(health,
  mAttackPower)` after `at_power_check`. Clean injection target.
- Whitelist v1 (revised): `E_AI`, `E_HM`, `E_FB`, `E_DF`, `E_YC`, `E_MD`
  (drop `E_GS`), plus one boss.

### 2.1 M2.5 correction — targeting-read coverage (D3) was overstated

The original v1 claim that the whitelist was "per-type verified" covered only
**combat/collider** surface (`mCcSph`/`mSph`/`m_ccCyl`/`defaultPowerType`), not
the **targeting-read** coverage implementation-plan Rev3 §5 M2 / risk 5 requires.
Two types read the player through `dComIfGp_getPlayer(0)` DIRECTLY (bypassing the
context-routed inlines `fopAcM_searchPlayer*` / `daPy_getPlayer*ActorClass`), so
host enemies aggroed slot 0 (the host's own Link), not the nearest remote player.
M2.5 routes every such read through the targeting context
(`fopAcM_getContextPlayer()` / `daPy_getPlayerActorClass()`, both →
`currentTargetPlayer()`, which falls back to slot-0 outside a scope):

- **E_YC** (Twilight Kargorok), 6 sites routed: `damage_check`,
  `e_yc_f_fly`, `e_yc_hovering`, `e_yc_attack`, `e_yc_wolfbite`,
  `daE_YC_Execute`. All under `#if TARGET_PC` with the original read in `#else`.
  Pointer types preserved (`daPy_py_c*` via `daPy_getPlayerActorClass()`;
  `fopAc_ac_c*` via `fopAcM_getContextPlayer()`) — no new cast noise.
- **E_AI** (Armos), 3 sites routed: `player_way_check`, `pl_check` (the attack
  LOS gate `other_bg_check(player)`), `damage_check`. `action()` already routed
  `m_angleToPlayer`/`m_playerDist` via `fopAcM_searchPlayer*` in M2.
- **E_HM / E_DF / E_MD / B_TN**: route cleanly already (their player reads go
  through the context-routed inlines); no direct slot-0 reads remain.

Post-M2.5 coverage: on TARGET_PC every direct read is routed — E_YC 6/6,
E_AI 3/3 sites; E_HM/E_DF/E_MD/B_TN had none. The only remaining
`dComIfGp_getPlayer(0)` occurrences in these TUs are the vanilla `#else`
branches (never compiled on PC; grep finds them, the preprocessor does not).
With a `ScopedEnemyTarget` pushed around the host enemy's execute, these reads
resolve the nearest real player; outside any scope the fallback is slot 0, so
the vanilla single-player path is untouched. D3 "enemies chase both players"
is now met for the full v1 whitelist.

### 2.2 M2.5 note — MAJOR-2 cullMtx shared-buffer fix

`puppetExecute` originally wrote one `static thread_local Mtx` and passed it to
`fopAcM_SetMtx`, which stores a POINTER — so with 2+ frozen puppets in a room all
culled against the LAST puppet's matrix (E_AI/E_HM/E_DF/E_YC/E_MD set
`fopAcStts_CULL_e`; culling is not disabled on PC). M2.5 gives each `EnemyEntry`
its own `Mtx cullMtx` member (stable per-entry address) and points
`actor->cullMtx = &entry->cullMtx` on every snapshot apply. Each puppet now culls
on its own position.

## 3. Boss candidate: `B_TN` (Twilit Igniter, Lakebed Temple)

- `cc_set` is public and self-contained (re-registers everything from model
  matrices) — usable as the client-side `refreshColliders`.
- Damage via public `damage_check()` which uses `cc_at_check` internally.
- Death grants a switch (`dComIfGs_onSwitch`) — the per-player flag-grant hook.
- `B_GM` (Armogohma) is too complex for v1 (13+ sph colliders, multi-phase
  demo) — defer.
- The 10/21 `cc_at_check` boss list from `03-enemies.md` (§6) stands as the
  candidate pool: `B_GND, B_GM, B_BQ, B_TN, B_MGN, B_OH`.

## 4. Open items the retry must still resolve

- `fopAc_Execute` freeze guard placement + `cullMtx` maintenance (risk 10).
- D3 targeting context swap (port the `local-coop-poc` ScopedContext mechanism;
  host-side enemy AI only).
- RelayPolicy additions in `session.cpp` (intent = no-relay, snapshot =
  owner→clients, result/event = relay) — the seam exists from M1.5.
- Drops (`fopAcM_createItemFromEnemyID` + death beat) and room-clear
  (`daAlldie_c` gating).
- The create-deadline (`kCreateDeadlineFrames`) is for puppets; enemies need
  their own spawn/stale lifecycle on clients (spawn on `EnemyEvent(spawn)`,
  delete on `EnemyEvent(died)`).

## 5. Retry resolution (M2 landed)

The M2 implementation resolved the open items; the deviations from this note
are recorded here.

- **Whitelist v1 (final, per-type verified): `E_AI`, `E_HM`, `E_DF`, `E_YC`,
  `E_MD`, `B_TN`.** `E_FB` is DROPPED like `E_GS`: `E_FB` is the Freezard
  (not the "Fire Baba" the earlier draft assumed); its `damage_check` only
  counts `AT_TYPE_IRON_BALL` hits, reads `dComIfGp_getPlayer(0)`, and casts
  `GetTgHitAc()` to `daObjCarry_c*` — a synthetic attacker cannot drive it
  safely. The remaining five enemies verified: `E_AI` hit-count semantics
  (`m_ccCyl`, drops 0x1E, `m_swbit` = param>>16); `E_HM` inline `At_Check`
  (`mSph`, drops 0x23, switch = param>>24); `E_DF` reaction-only (bounds;
  never dies from damage — `mCyl`); `E_YC` wolf-bite-only flying enemy
  (`mCcSph`); `E_MD` break-path suit of armor (`mCyl`, no HP death); `B_TN`
  Darknut mini-boss via shared `cc_at_check` (`mSphA/B/C` injection, switch =
  param&0xFF, drops 29).
- **Private-member access**: `m_ccCyl`/`mSph`/`mSphA/B/C`/`m_modelMorf`/
  `mAnm_p`/`mpModelMorf2` are private in their classes; the injection and
  drive paths use the offsets pinned by each header's `STATIC_ASSERT`
  (03-enemies.md §7.2's `colliderOffsets` idea). `mFlags` on `fopEn_enemy_c`
  is public.
- **Multi-hit batching**: v1 selects the STRONGEST intent per enemy per frame
  and injects it once — the enemy's own handler runs its authentic reaction.
  Same-frame multi-hits on one enemy are lossy in vanilla too (a single Tg
  hit flag; `cCcS::ChkAtTg` clears flags at the start of every collision
  pass), so no sum model is needed; the CombatResult reports the applied hit.
- **Death detection**: the died event fires when the actor is GONE
  (`fopAcM_SearchByID` backstop) so the client's puppet deletion keeps ALLDIE
  scans in step; the `isDead` (health<=0) window during a death anim captures
  the per-player switch before the delete. `E_AI`'s death anim (~56 frames)
  has no public isDead signal — clients keep the puppet posed until the died
  event (accepted visual lag, recorded in the milestone summary).
- **`cc_at_check` multipliers**: confirmed — `at_power_check` applies no
  Link multipliers; the slot-0 multiplier block lives in `cc_at_check`
  (d_cc_uty.cpp ~434), so synthetic hits against `cc_at_check` enemies get
  the OWNER's equipment multipliers (documented v1 limitation, unchanged).
- **Synthetic At collider**: a `dCcD_Sph` with its `dCcD_GObjInf` populated
  (Se/Mtrl/HitMark) + `cCcD_Stts` with the attacker puppet; the per-entry
  storage (one synth per registered enemy) avoids same-frame cross-enemy
  corruption of the hit object.
- **Room-clear**: died-at-delete keeps ALLDIE in step for stage-placed
  enemies; the reliable per-room `EnemyEvent(RoomClear)` bit is sent by the
  host's `daAlldie_c` hook and gates the client's ACT_CHECK -> ACT_TIMER
  transition for rooms that had synced enemies.
- **Bosses (D9)**: `B_TN` is an ordinary wire enemy; its died event carries
  `mSwitchNo` (param&0xFF) and each client grants it to its OWN save
  (`dComIfGs_onSwitch`) — per-player story intact. v1 fallback: the boss
  exists iff the owner's story spawns it (no union bit).
