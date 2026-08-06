# Adversarial review — M2 (enemy authority)

Reviewer: `z-ai/glm-5.2` (adversarial reviewer, M2 enemy authority).
Branch: `net-coop`. Review range: `9aff5a879a..HEAD`
(commits `9af6d2e256` wire v4 + relay policy, `c6b90e6727` registry/snapshots/targeting/combat,
`bb235ed585` TARGET_PC attach points, `5d7954af6b` docs/resolutions, `008ff08ae8` EnemyEvent flagMask + hit-position).
Aurora submodule intentionally at upstream `6c4c27f9` — not a regression.

Method: read design docs (implementation-plan Rev3 §5/§3/§6, network.md, 03-enemies.md,
m2-design-notes.md, 00-network.md §5/§2), diffed the M2 range, verified private-member
offsets against headers, forced a rebuild of every touched TU + the full game, ran
`dusk_net_selftest`, and traced the combat/cull/freeze paths against the vanilla call
graph. No source modified; only this review written.

---

## VERDICT

**Substantially meets the acceptance criteria, but NOT a clean accept — two MAJORS
must be fixed before M3.**

| Acceptance criterion (plan §5 M2) | Met? | Evidence |
|---|---|---|
| Host sims enemies | ✅ | host `hostOnExecuted` + native sim; `puppetExecute` no-op on host (`coop_enemy.cpp`) |
| Client sees same HP | ✅ | `EnemySnapshot.hp/maxHp` applied in `puppetExecute`; selftest relay ok |
| Client hits land and kill | ✅ | `noteAtTgHit`→`CombatIntent`→owner `flushHostIntents`+`SetTgHitSynthetic` (full `dCcD_Sph` w/ `dCcD_GObjInf`); selftest |
| Drops spawn on both | ✅ | client `OnEnemyDied`→`fopAcM_createItemFromEnemyID`+`fopAcM_createDisappear`; host native; no double-spawn (freeze + `Dying` guard) |
| Doors open together | ✅ | `daAlldie_c` `hostRoomCleared`/`clientRoomClearGated` + per-room `EnemyEvent(RoomClear)` |
| **Enemies chase both players (D3)** | ⚠️ **PARTIAL** | E_YC chase/attack reads `dComIfGp_getPlayer(0)` directly (6 sites); E_AI attack-gate reads slot-0 (3 sites) — see MAJOR-1 |
| Boss flag per-player | ✅ | `EnemyEvent(died, flagMask)`→client `dComIfGs_onSwitch(sw, roomNo)` on its own save; `B_TN` `flagMask=param&0xFF` |

Build: full game (`dusklight` target) builds clean under a forced rebuild of every M2
TU (`coop_enemy/combat/context.cpp`, `coop.cpp`, `protocol.cpp`, `session.cpp`,
`selftest_main.cpp`, `f_op_actor.cpp`, `d_cc_s.cpp`, `d_a_alldie.cpp`, and all 6
whitelisted enemy TUs + `d_a_alink.cpp`/`d_cc_uty.cpp` touched to force recompile).
Only pre-existing JSystem `std::iterator` deprecation warnings — no new warnings/errors.
`dusk_net_selftest` builds and **`PASS: all checks succeeded`**, including the new
`RunM2RelayPolicyCheck` (all 6 relay-policy assertions). M1 player-replication paths
compile unchanged (`d_a_alink.cpp`, `d_cc_uty.cpp` are not in the M2 diff — M1.5 intact).

---

## RANKED FINDINGS

### BLOCKER
None. The build is clean, the wire is correct, the relay seam is behavior-correct
(selftest-verified), and the combat injection path avoids the NULL-`GObjInf` crash trap.

### MAJOR-1 — D3 targeting not ported for E_YC (and only half for E_AI); "enemies chase both players" is overstated

The D3 headline claims host enemies aggro the NEAREST real player. The port routes
only `fopAcM_searchPlayer*` and `daPy_getPlayer*ActorClass` through
`currentTargetPlayer()` (include/f_op/f_op_actor_mng.h, include/d/actor/d_a_player.h).
But the whitelisted enemies' movement/attack AI also reads **`dComIfGp_getPlayer(0)`
directly**, which the M2 port does NOT route — those still resolve slot 0 (the host's
Link):

- **E_YC (Twilight Kargorok)** — 6 direct slot-0 reads vs 2 context-routed. The chase
  path is direct: `e_yc_attack` sets `mTargetPos = player->current.pos` from
  `dComIfGp_getPlayer(0)` (src/d/actor/d_a_e_yc.cpp:309,315); `e_yc_f_fly` reads
  `player->current.pos.y` from slot-0 (:192,221); `e_yc_hovering` (:271),
  `daE_YC_Execute` (:614), `damage_check` (:57), `e_yc_wolfbite` (:429) all read
  `dComIfGp_getPlayer(0)`. **E_YC flies toward and attacks the host's Link position,
  not the nearest remote player.** The 2 routed `fopAcM_searchPlayer*` calls are not
  the primary targeting path.
- **E_AI (Armos)** — `action()` does route `m_angleToPlayer`/`m_playerDist` via
  `fopAcM_searchPlayer*` (d_a_e_ai.cpp:666-667), so it turns toward the nearest
  player. But `pl_check` (the attack LOS gate) uses `dComIfGp_getPlayer(0)` for
  `other_bg_check(player)` (:170-174), and `player_way_check` (facing) reads slot-0
  (:137), and `damage_check` reads slot-0 (:183). So E_AI turns toward the nearest
  player but only attacks when the **host's Link** is in LOS/facing.

The m2-design-notes §2 "Retry resolution" claims the whitelist was "per-type
verified," but that verification covered only the **combat/collider** surface
(`mCcSph`/`mSph`/`m_ccCyl` + `defaultPowerType`), not the **targeting-read** coverage
that implementation-plan §5 M2 / risk 5 explicitly required ("Per-type verification
that the context swap covers each whitelisted enemy's reads"). The gap is
**undocumented** in the design notes (which present D3 as done).

Evidence: `grep -c "dComIfGp_getPlayer(0)"` per whitelisted TU — E_YC:6, E_AI:3,
E_HM:0, E_DF:0, E_MD:0, B_TN:0 (direct, non-routed). E_HM/E_DF/E_MD/B_TN route
cleanly through the inlines (their reads are covered); E_YC and E_AI are the gaps.

**Fix (pick one, before M3):**
1. Route the enemy-AI `dComIfGp_getPlayer(0)` reads in `d_a_e_yc.cpp`/`d_a_e_ai.cpp`
   through `fopAcM_getContextPlayer()`/`currentTargetPlayer()` (per-site, like the POC
   did for `dComIfGp_getLinkPlayer()`), **or**
2. Drop E_YC (and note E_AI's partial coverage) from the v1 "chase both players"
   claim and document it explicitly as a v1 limitation (risk-5 fallback), **or**
3. Add per-type AI hooks (plan risk-5 fallback) for the uncovered reads.

At minimum the gap must be **documented** — right now D3 is claimed as complete and
it is not.

### MAJOR-2 — cullMtx is a shared static buffer; multi-puppet rooms cull on the LAST puppet's position

`puppetExecute` (src/dusk/coop/coop_enemy.cpp) does:

```cpp
static thread_local Mtx s_puppetCullMtx;
mDoMtx_stack_c::transS(actor->current.pos.x, ...);
std::memcpy(s_puppetCullMtx, mDoMtx_stack_c::get(), sizeof(Mtx));
fopAcM_SetMtx(actor, s_puppetCullMtx);
```

`fopAcM_SetMtx` stores a **pointer** (`actor->cullMtx = m`; include/f_op/f_op_actor_mng.h:308),
not a copy. So every frozen puppet's `cullMtx` points at the same `s_puppetCullMtx`,
which holds the **last** `puppetExecute` call's matrix. E_AI and E_HM profiles set
`fopAcStts_CULL_e` (src/d/actor/d_a_e_ai.cpp:965, d_a_e_hm.cpp:1617), so `fopAc_Draw`
(src/f_op/f_op_actor.cpp:254) runs `fopAcM_cullingCheck`, which reads
`fopAcM_GetMtx(actor)` (the shared pointer). In a room with 2+ frozen puppets, all
cull against the last puppet's frustum position: an on-screen puppet vanishes when
the last puppet is off-screen, and an off-screen one renders when the last is on-screen.

03-enemies.md §1.3 caveat 1 explicitly recommended hooking
`fopAcM_cullingCheck` → `FALSE` for tracked puppets (or relying on the fork disabling
culling — but this codebase does **not** disable culling: `fopAcM_cullingCheck`
(src/f_op/f_op_actor_mng.cpp:1023) has no `#if TARGET_PC return FALSE` early-out).
The "cullMtx maintained (risk 10)" mitigation in the plan is implemented but
incorrect for the multi-puppet case because of the shared buffer.

**Fix (before M3):** give each `EnemyEntry` its own `Mtx cullMtx` member and point
`actor->cullMtx` at `&e->cullMtx` (stable per-entry address), **or** add
`if (dusk::coop::enemy::isRegistered(i_actor)) return FALSE;` to
`fopAcM_cullingCheck` under `#if TARGET_PC` (the 03-enemies.md recommendation).
Single-puppet rooms are unaffected; the bug only manifests with 2+ whitelisted
enemies in one room (e.g. a pair of Armos), so it is visual-only but real.

### MINOR-3 — `cc_at_check` slot-0-multiplier v1 limitation is not documented in code

The m2-design-notes §5 documents that synthetic hits against `cc_at_check` enemies
get the **owner's** equipment multipliers (the slot-0 block at
src/d/d_cc_uty.cpp:428-432, `player_p = dComIfGp_getPlayer(0)` → `getSwordAtUpTime`
×2), so remote players' damage vs `cc_at_check` enemies (E_HM, B_TN) does not carry
their own equipment multipliers. This is the documented v1 limitation — but it is
documented only in the design notes, **not at the code site**. Review item #5
expected it in code.

**Fix:** add a one-line comment near d_cc_uty.cpp:428, e.g.
`// M2 v1: remote-attacker equipment multipliers not applied (slot-0 read); owner
// reproduces damage deterministically — m2-design-notes §5`.

### MINOR-4 — `fopAc_Execute` lambda wrapper is not byte-identical on the non-PC path

src/f_op/f_op_actor.cpp wraps `fpcMtd_Execute` in an immediately-invoked lambda:
`ret = [&](){ #if TARGET_PC ... #endif return fpcMtd_Execute(...); }();`.
Only the `std::optional<ScopedEnemyTarget>` body is `#if TARGET_PC`-guarded; the
lambda itself is always present. Semantically identical (immediately invoked,
returns the same value) and inlines identically at `-O2`, but the non-PC path is
no longer the original direct `ret = fpcMtd_Execute(...)` — the conventions
("non-PC build stays vanilla / byte-identical") are technically broken.

**Fix:** guard the whole lambda under `#if TARGET_PC` with a plain
`ret = fpcMtd_Execute((process_method_class DUSK_CONST*)actor->sub_method, actor);`
in the `#else`, so the non-PC path is the original one-liner.

### MINOR-5 — `EnemySnapshot.angle` is applied to both `current.angle.y` and `shape_angle.y`; host sends only `shape_angle.y`

`SendSnapshot` reads `actor->shape_angle.y` into `snap.angle`; `puppetExecute` writes
it to **both** `actor->current.angle.y` and `actor->shape_angle.y`. If a type's
`current.angle.y` ≠ `shape_angle.y` on the host, the client's `current.angle.y` is
the host's `shape_angle.y` (a minor physics/interp mismatch). Most enemies rotate the
whole actor so the two coincide; v1 yaw-only is documented (00-network §5). Low impact.
**Fix (optional):** send `current.angle.y` separately or document the
`current.angle.y == shape_angle.y` assumption for the whitelist.

### MINOR-6 — `RemoteInRoom()` does not scope by the remote's actual room

`RemoteInRoom()` (coop_enemy.cpp) returns true if any remote roster slot is present,
not same-room. Correct for v1 co-located (one room); v2 room-ownership will need
same-room scoping or the host will snapshot enemies for remotes in other rooms.
**Note for v2**, not a v1 bug.

### MINOR-7 — `hostRoomCleared` single-room latch

`g_hostRoomClearSent`/`g_hostRoomClearRoom` is a one-room latch reset only by
`ClearAll()` on room change. Fine for monotonic room progression; won't re-send if a
room re-cleared after a later room cleared. Not a v1 scenario (stage enemies don't
respawn). Dormant.

---

## VERIFIED-OK

1. **Build & regression**: full `dusklight` builds clean under forced rebuild of all
   touched TUs (only pre-existing JSystem deprecation warnings). `dusk_net_selftest`
   `PASS` incl. `RunM2RelayPolicyCheck` (all 6 assertions). M1 player-replication
   paths compile (`d_a_alink.cpp`, `d_cc_uty.cpp` unchanged in the M2 range).
2. **Wire v4** (`protocol.h`/`protocol.cpp`): `kProtocolVersion` bumped 3→4.
   `EnemySnapshot` 28→44 B (added `speed` Vec3f + `semantics` u8 + `reserved[3]`);
   `EnemyEvent` 6→10 B (added `flagMask` u8 + `reserved[3]`); `CombatIntent` rebuilt
   (raw `atp`/`atType`/`powerType`/`hitType`/`computedPower` + `hitPos`/`attackerPos`,
   42 B). `WireSize` / serialize / deserialize match the struct field order exactly;
   `JoinRequest` carries version → mismatches rejected.
3. **Relay policy** (`session.cpp`): `PolicyFor` returns `None` for
   EnemySnapshot/EnemyEvent/CombatIntent/CombatResult; `HandleData` calls `gameHandler_`
   without `ForwardGameMessage` (not re-broadcast). Host's own broadcasts go via
   `SendGameMessage`→`SendToAll`. CombatIntent is NOT relayed to other clients;
   EnemySnapshot owner→clients only; CombatResult/EnemyEvent simulcast; a buggy
   client's EnemySnapshot/CombatResult is consumed but not echoed. The seam does not
   leak M1's PlayerState/PlayerEvent star path. Selftest verifies all six cases.
4. **Freeze** (`fopAc_Execute`, coop_enemy.cpp `puppetExecute`): pre-guard returns
   true → vanilla dispatch skipped (AI/action state machine never runs → divergence
   impossible; no ghost side effects). Draw is a separate dispatch and keeps running.
   `old = current` set after apply. Colliders re-registered each frame via
   `adapter->refreshColliders` (dCcS clears registrations every frame). Host path:
   `IsHost()` → `puppetExecute` returns false immediately → native sim.
5. **D3 context push placement** (`f_op_actor.cpp`): the `ScopedEnemyTarget` is
   constructed only when `hostNeedsContext(actor)` (whitelisted + registered + live
   session + host) and destructed at the end of the lambda — pushed ONLY around the
   enemy AI execute on the host, never on clients, never globally. No double-push/leak
   (RAII; `thread_local std::vector` stack). Non-enemy actors never get the scope
   (`isRegistered` requires the actor be in the enemy registry). Camera/NPC/other
   consumers outside the scope read `currentTargetPlayer()` → `g_stack.empty()` →
   `dComIfGp_getPlayer(0)` (vanilla). **(Caveat: the covered-read requirement is only
   met for E_HM/E_DF/E_MD/B_TN; E_YC and E_AI have uncovered direct reads — MAJOR-1.)**
6. **Combat validation** (`coop_combat.cpp`, `coop_enemy.cpp`): client intercept at
   `dCcS::SetAtTgGObjInf` (`d_cc_s.cpp` TARGET_PC) → `noteAtTgHit` only for the local
   real Link (`atActor == dComIfGp_getPlayer(0)`) attacking a registered enemy puppet;
   host no-op (native). `CombatIntent` carries the raw attack fields. Owner validates
   (attacker present, no friendly fire, target exists/registered, range sanity) and
   queues per-enemy. `flushHostIntents` (in `onGameFrame`, **before** the actor phase
   — verified `dusk::coop::onGameFrame()` at f_ap_game.cpp:826 runs before
   `fapGm_Execute` at :830) injects the strongest intent per enemy/frame via
   `SetTgHitSynthetic` — a full `dCcD_Sph` with its `dCcD_GObjInf` populated
   (`SetAtSe`/`SetAtMtrl`/`SetAtHitMark`, `SetAtAtp`/`SetAtType`, `SetStts`+`SetActor`,
   `SetTgHitPos`), avoiding the NULL-`GObjInf` crash trap (`GetGObjInf` is virtual on
   `dCcD_GObjInf`; a raw `cCcD_Obj` returns NULL — verified the synthetic is a
   `dCcD_Sph` which inherits it). The enemy's own next execute polls `ChkTgHit()` and
   runs its authentic damage handler. `hostOnExecuted` sends `CombatResult` with
   post-execute HP via the `pendingHit` latch. Multi-hit batching = strongest `atp`
   per enemy/frame (vanilla is lossy for same-frame multi-hits on one Tg flag —
   documented). The `cc_at_check` slot-0 multiplier limitation is real (MINOR-3) but
   the deterministic `at_power_check` path (`atp`+`mPowerType`, no Link multipliers)
   reproduces the client's damage for inline handlers (E_HM `At_Check`, B_TN
   `damage_check`).
7. **Drops** (D5): on `EnemyEvent(died)`, client `OnEnemyDied`→`SpawnDropAndBeat` calls
   `fopAcM_createItemFromEnemyID(dropTableId, &pos, -1,-1, NULL,NULL,NULL,NULL)` +
   `fopAcM_createDisappear(actor, &pos, beatSize, 0, 0xFF)` (signatures verified) under
   the play-scene layer restore. Host un-gated (authoritative; its enemy's own death
   spawns the native drop). No double-spawn: the frozen puppet never runs its own death
   handler, and `OnEnemyDied` guards `state == Dying`. Puppet deleted after an 18-frame
   death beat (`fopAcM_delete`).
8. **Room-clear** (`d_a_alldie.cpp`): `actionCheck` calls `hostRoomCleared(roomNo)`
   when the scan is empty, and `clientRoomClearGated(roomNo)` holds the
   `ACT_CHECK→ACT_TIMER` transition for rooms that had synced enemies
   (`g_roomHadEnemies`) until the owner's `EnemyEvent(RoomClear)` bit arrives;
   `actionTimer` re-checks the gate against a rescan race. Doors open together for
   stage-placed enemies (died-event puppet deletion keeps both scans in step).
9. **Bosses** (D9): `B_TN` is an ordinary wire enemy (`isBoss=true`,
   `DamageSemantics::Hp`, `dropTableId=29`); `tnDeathSwitch = fopAcM_GetParam(actor) &
   0xFF`; on death, `EnemyEvent(died, flagMask=switch)` → each client
   `GrantLocalSwitch`→`dComIfGs_onSwitch(sw, roomNo)` on its OWN save (per-player
   story intact). v1 fallback: boss exists iff the owner's story spawns it (no union
   bit). `B_TN` `cc_set`/`mtx_set` are public/self-contained; `damage_check`→`cc_at_check`.
10. **Entity-id stability** (`StageKey`): `(roomNo & 0xFF) << 8 | (setID & 0xFF)`,
    `setID < 0x100`, `roomNo ≥ 0` → stage keys 0x0000-0x7Fxx; dynamic counter starts at
    `0x8000` (no collision with stage space); `onRoomUnload`/`ClearAll` resets per-stage
    registries on room/stage change. `kProtocolVersion` bumped for the
    EnemySnapshot/EnemyEvent/CombatIntent wire changes.
11. **Private-member offsets** (verified against each header's `STATIC_ASSERT`-pinned
    layout): E_AI `m_modelMorf`@0x5D0, `m_ccCyl`@0xBC8 (d_a_e_ai.h); E_HM
    `mAnm_p`@0x618, `mSph`@0x928 (d_a_e_hm.h); E_DF `mpMorfSO`@0x5C8, `mCyl`@0x6B8;
    E_YC `mpMorf`@0x5B8, `mCcSph`@0x908; E_MD `mpModelMorf`@0x68C, `mCyl`@0x8EC; B_TN
    `mpModelMorf2`@0x600, `mSphA`@0x2EC8, `mSphB`@0x3270, `mSphC`@0x3618,
    `sizeof(dCcD_Sph)==0x138` (d_cc_d.h:490). All offsets match the headers — ABI-safe.
    `mFlags` (u16, 0x58E) and `fopEn_flag_Dead` are public on `fopEn_enemy_c`.
12. **Conventions/scope**: all attachment points are additive `#if TARGET_PC` with
    vanilla `#else`; `d_cc_s.cpp`/`d_a_alldie.cpp`/`f_op_actor.cpp` guards are clean.
    No M3 (time/weather) or M4 (session-polish) scope creep — only enemy/combat/room-
    clear. Commit hygiene: 5 logical commits (wire v4 → game side → attach points →
    docs → flagMask refinement). `d_cc_uty.cpp` was **not** modified in M2 (the
    `at_power_check` path is the existing public function — no hook needed); the
    `cc_at_check` slot-0 block is unchanged from M1.

---

## MUST-FIX before M3

1. **MAJOR-1 (D3 targeting gap)**: cover the direct `dComIfGp_getPlayer(0)` reads in
   `d_a_e_yc.cpp` (6 sites) and `d_a_e_ai.cpp` (3 sites: `pl_check`, `player_way_check`,
   `damage_check`) via the context, **or** drop E_YC from the v1 whitelist / document
   E_AI's partial coverage as an accepted v1 limitation. The "enemies chase both
   players" acceptance criterion is currently false for E_YC and partial for E_AI;
   the m2-design-notes "per-type verified" claim overstates this. This is the
   headline D3 item — it must be either fixed or explicitly re-scoped.
2. **MAJOR-2 (cullMtx shared buffer)**: give each registered puppet its own `cullMtx`
   storage (point `actor->cullMtx` at a per-`EnemyEntry` `Mtx`), **or** add
   `if (dusk::coop::enemy::isRegistered(i_actor)) return FALSE;` to
   `fopAcM_cullingCheck` under `#if TARGET_PC`. Without this, rooms with 2+ frozen
   whitelisted enemies have puppets that pop/vanish based on the last puppet's frustum
   position (E_AI/E_HM profiles set `fopAcStts_CULL_e`; culling is NOT disabled on PC
   in this codebase).

**Recommended (cheap, before M3 but not blocking):** MINOR-3 (document the
`cc_at_check` slot-0 limitation at d_cc_uty.cpp:428) and MINOR-4 (guard the
`fopAc_Execute` lambda so the non-PC path is byte-identical). MINOR-5/6/7 are
v2/polish and can ride.

---
