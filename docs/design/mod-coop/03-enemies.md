# Enemy authority — host-authoritative enemies from a pure `.dusk` mod

Status: **Investigation** — feeds the consolidated implementation plan.
Companion: `docs/design/network.md` (authority model), `docs/design/mod-coop/00-network.md` (transport/protocol surface).

Scope: how a **pure mod** (stock Dusklight, no fork-only symbols, hook by symbol
name only) makes enemies host-authoritative: freeze client-side enemy AI,
render host-driven puppets, validate combat against the sim owner, keep
room-clear consistent, and treat bosses as "enemies + story policy".

---

## 1. The actor update path and the generic AI-freeze hook point

### 1.1 Frame order (what runs when)

```
fapGm_Execute                                src/f_ap/f_ap_game.cpp:824
└─ fpcM_Management(fapGm_Before, fapGm_AfterRecord)
   ├─ fpcEx_Handler(fpcM_Execute)            ← ALL process executes, incl. every actor
   │   └─ fopAc_Execute(actor)               src/f_op/f_op_actor.cpp  (per-actor gate)
   │       └─ fpcMtd_Execute(actor->sub_method, actor)   → e.g. daE_AI_Execute → e_ai_class::Execute()
   ├─ fpcDw_Handler(fpcM_Draw)               ← ALL draws
   │   └─ dScnPly_Draw → dComIfG_Ccsp()->Move()   src/d/d_s_play.cpp:564, src/d/d_cc_s.cpp:762
   │       └─ cCcS::Move: At/Tg/Co pair checks → dCcS::SetAtTgGObjInf (hit registration)
   └─ fapGm_AfterRecord
```

Key consequence: **collision pair processing (where hits are registered) runs in
the draw phase, after actor executes**. An enemy's `ChkTgHit()` poll therefore
consumes hits registered in the *previous* frame's collision pass. For the
network mod this is convenient: client intent capture and owner-side apply both
have a natural one-frame boundary to work against.

### 1.2 `fopAc_Execute` — the framework-level per-actor gate

`fopAc_Execute` (file-local `static int fopAc_Execute(void*)` in
`src/f_op/f_op_actor.cpp`, line 292) is the single dispatch point every actor
passes through every frame. It already gates execution on:

- global pauses (`dComIfGp_isPauseFlag`, `dScnPly_c::isPause`, `dComIfA_PauseCheck`);
- `daSus_c::check(actor)` — suspend boxes set status `fopAcStts_UNK_0x20000000_e`;
- `dComIfGp_event_moveApproval(actor)` — event/demo freeze;
- `fopAc_ac_c::getStopStatus()` — global stop mask;
- `fopAcStts_NOEXEC_e` (status) combined with `fopAcCnd_NODRAW_e` (condition).

When execution is skipped, the game sets `fopAcCnd_NODRAW_e` and calls
`eventInfo.suspendProc(actor)`. Drawing is gated separately in `fopAc_Draw`.

The fork already treats this function as the generic choke point: the
`#if TARGET_PC` NPC-context block at the top of `fopAc_Execute` pushes a
`ScopedContext` before the per-actor dispatch.

### 1.3 Verdict: the generic freeze point

There is **no vanilla per-actor "freeze AI but keep drawing" flag**:

| Mechanism | Freezes execute | Keeps draw | Notes |
|---|---|---|---|
| `fopAcStts_NOEXEC_e` + `fopAcCnd_NODRAW_e` | yes | **no** | profile-level; used by props |
| `fopAcStts_UNK_0x20000000_e` (suspend) | yes | no | managed by `daSus_c` boxes; blocks draw too |
| `event_moveApproval` | yes | no | demo/staff freeze |
| **mod pre-hook on `fopAc_Execute` → `HOOK_SKIP_ORIGINAL`** | yes | **yes** | the clean choke point |

**The generic hook point is a pre-hook on `fopAc_Execute`**
(`DEFINE_HOOK_SYMBOL("fopAc_Execute", int(void*), ...)` — file-local static,
present in the symbol manifest):

```cpp
HookAction on_fopAc_execute_pre(ModContext*, void* args, void*, void*) {
    auto* actor = mods::arg<fopAc_ac_c*>(args, 0);
    if (!enemy_registry::isPuppet(actor)) { return HOOK_CONTINUE; }
    enemy_registry::applySnapshot(actor);   // base fields + per-type drive (§3, §7)
    return HOOK_SKIP_ORIGINAL;              // AI never runs → divergence impossible
}
```

Why this is sufficient and safe:

- **AI suppression is total** — the enemy's own execute (its action state
  machine, physics, animation, sounds, event calls) never runs on the client.
- **Local damage suppression comes for free** — the frozen enemy never polls
  `ChkTgHit()` and never calls its `At_Check`/`cc_at_check`, so its HP is never
  decremented locally (§4). The player's sword still *contacts* the puppet
  (colliders stay registered), so the attacker gets hit-stop/hitmark feedback
  and the mod can capture the intent at the collider level.
- **Drawing keeps working** — `fopAc_Draw` is a separate dispatch; as long as
  the mod never sets `NODRAW`, the enemy's `Draw()` runs every frame. The draw
  only reads `current.pos`, `tevStr`, and the model state — all of which the
  snapshot apply drives (§1.5).
- **The skip decision can be made from one map lookup** (entity id → puppet),
  so non-puppet actors pass through at near-zero cost.

Two caveats to handle in the mod:

1. **Culling**: the fork disables culling entirely (`fopAcM_cullingCheck`
   returns `FALSE` under `TARGET_PC`, `src/f_op/f_op_actor_mng.cpp:1040`)
   because the single draw pass tests one camera frustum. A frozen puppet's
   `cullMtx` is stale (it is set inside the actor's own execute via
   `fopAcM_SetMtx`), so on stock the mod should hook `fopAcM_cullingCheck`
   (public, `namespace fopAcM`) to return `FALSE` for tracked puppets, or rely
   on the same per-view problem the fork documents. Note `fopAc_Execute` also
   performs the "delete if fell out of world" check
   (`home.pos.y - current.pos.y > 5000` when status 0x20 is set) — a puppet
   driven by snapshot state must not trip it (its `home` is untouched, so it
   won't).
2. **`old = current`**: the vanilla execute copies `actor->old = actor->current`
   before dispatching. For puppets the mod should copy `old = current` after
   applying the snapshot so physics/interpolation consumers see a coherent
   delta.

### 1.4 Per-actor dispatch chain (for completeness)

`fopAc_Execute` → `fpcMtd_Execute(actor->sub_method, actor)` →
`daE_AI_Execute` → `e_ai_class::Execute()` (`src/d/actor/d_a_e_ai.cpp:761`),
which calls `setCcCylinder()` (collider registration) then `action()` — the
state machine switch on `m_action` (`WAIT/MOVE/ATTACK/DAMAGE/RETURN`) — then
physics (`speed += gravity; current.pos += speed; m_acch.CrrPos(...)`), then
`setBaseMtx()` (model matrix). Every enemy actor follows this shape; only the
member names and offsets differ.

### 1.5 The per-type "drive" (model + colliders) — the one genuinely per-type bit

Skipping execute means the mod must reproduce the parts the engine would have
run. Two of them are per-type, both solvable with public members:

- **Collider registration** — enemies register their damage colliders each
  execute (`e_ai_class::setCcCylinder()` calls `dComIfG_Ccsp()->Set(&m_ccCyl)`).
  A frozen puppet that never registers its collider becomes intangible — the
  player's sword passes through and **no combat intent can ever originate**.
  Per whitelisted type the adapter calls the public refresh member directly
  (e.g. `static_cast<e_ai_class*>(a)->setCcCylinder()`); the mod includes the
  game headers, so this is plain C++.
- **Model matrix + animation** — `e_ai_class::setBaseMtx()` is a public member
  doing `transS(current.pos)` + `YrotM(shape_angle.y)` + `scaleM` +
  `setBaseTRMtx` + `m_modelMorf->modelCalc()`. After applying snapshot
  pos/angle, the adapter calls it, then drives the animation (`m_anm` id +
  `m_modelMorf->setFrame(...)`). `Draw()` then renders correctly
  (`e_ai_class::Draw()` only reads `current.pos`, `tevStr`, `m_brk`,
  `m_modelMorf`).

**Verdict summary**: the *freeze* is fully generic (one hook); the *apply* is
generic at the base-class level (pos/angle/speed/health/flags) plus a small
per-type table (model/animation/collider offsets + refresh calls) — the same
shape as the fork's `EnemyAdapter` whitelist. See §7.

---

## 2. The in-tree enemy subsystem as reference

`include/dusk/coop/coop_enemy.h` + `src/dusk/coop/coop_enemy.cpp`:

- **`EnemyAdapter`** — whitelist entry per procName: `whitelistClone`,
  `maxClonesPerSource/Room`, `allowProgressionSwitch`, capsule params,
  spawn offset. Gate G ships exactly one entry (Armos `E_AI`); the network
  mod needs the same whitelist concept with an extended schema (§7).
- **`noteEligibleSource`** — queues a *stage-placed* enemy
  (`setID != 0xFFFF`, never a clone) for augmentation; called from the enemy's
  own `Create` (`d_a_e_ai.cpp:944`) and a periodic
  `fopAcIt_Executor(scanEligibleSources)` scan in `tick()`.
- **`spawnClone`** — dynamic spawn via `fopAcM_create` with the play-scene
  layer trick (`fpcLy_SetCurrentLayer(&playScene->layer)`); params sanitized
  (`sanitizeArmosParameters` strips the death-switch byte); placement probed
  (ground/hazard/water/roof/capsule clearance). This is the pattern for
  network-driven dynamic spawns (`EnemyEvent(spawn)`).
- **`roomClearBlocked()`** — ALLDIE must wait while clones are pending
  (`d_a_alldie.cpp` `coopRoomStillHasEnemies`). The network version extends
  this same hook (§5).
- **HP scaling** — `applySnapshottedHp` writes `field_0x560` (max) + `health`
  from an encounter multiplier; confirms `field_0x560` is the base/max HP
  field on `fopAc_ac_c`.

Reusable for the mod: the entity-match rule
(**`(roomNo, setID, procName)` → actor**, `setID == 0xFFFF` ⇒ dynamic spawn),
the adapter registry (`findAdapter`), the play-scene-layer spawn trick, and the
`roomClearBlocked` ALLDIE hook.

---

## 3. Enemy identity and the state surface

### 3.1 Identity

- **Group**: `fopAcM_GetGroup(actor) == fopAc_ENEMY_e` (2) — the generic
  "is an enemy" test (used by `fopAcM_myRoomSearchEnemy`'s judge and the fork's
  source scan). Bosses are group 2 as well.
- **Type**: `fopAcM_GetName(actor)` / `fpcM_GetName` → `s16` procName
  (`fpcNm_E_AI_e` …). 95 `fpcNm_E_*` + 21 `fpcNm_B_*` profiles
  (`include/f_pc/f_pc_name.h`).
- **Instance**: `fpcM_GetID` (proc id), `fopAcM_GetSetId` (`setID`, stage
  placement id; `0xFFFF` = dynamic), `fopAcM_GetRoomNo`.

**Network entity id** = assigned by the sim owner: stage-placed enemies key by
`(roomNo, setID, procName)`; dynamic spawns get a monotonic counter. Clients
map entity id → local actor: stage enemies by the same key (every machine has
the same stage), dynamic ones created on `EnemyEvent(spawn)`.

### 3.2 Base-class fields (all `fopAc_ac_c`, `include/f_op/f_op_actor.h`)

| Field | Offset | Snapshot use |
|---|---|---|
| `current.pos` / `current.angle` / `old` | 0x4D0 | transform |
| `shape_angle` | 0x4E4 | facing (draw) |
| `speed` / `speedF` / `gravity` | 0x4F8 / 0x52C / 0x530 | physics hint |
| `health` | 0x562 | **current HP** (this is what `cc_at_check` decrements) |
| `field_0x560` | 0x560 | base/max HP (per-type: Armos resets both to 1000 every frame) |
| `actor_status` / `actor_condition` | 0x49C / 0x4A0 | `fopAcStts_BOSS_e` (1<<26), freeze/cull bits |
| `attention_info.position/flags` | 0x544 | aggro display; `fopAc_AttnFlag_BATTLE_e` |
| `eyePos` | 0x538 | sound/effect anchor |

`fopEn_enemy_c` (base for *all* enemies and bosses, +0x0x568) adds the generic
**phase bits**: `mFlags` (0x58E: `fopEn_flag_Dead/Down/CutDownHit/WolfBite/HeadLock…`),
`mDownPos`, `mHeadLockPos`, `mThrowMode`, `mAnmFrame` — a portable "is it dead /
downed / wolf-bitten" byte for the snapshot.

### 3.3 Per-type state (representative: Armos + Torch Slug + a boss)

`e_ai_class` (Armos, `include/d/actor/d_a_e_ai.h`):
`m_action` (s16, 0x68E), `m_mode` (0x690), `m_anm` (int, 0x684 — current anim
id), `m_modelMorf` (`mDoExt_McaMorfSO*`, 0x5D0 — frame lives here), `m_timers[4]`,
`m_invulnerabilityTimer`, `m_hitCount`, `m_atInfo` (`dCcU_AtInfo`), colliders
`m_ccAtSph` / `m_ccShieldSph` / `m_ccCyl` (0x958/0xA90/0xBC8).

`daE_HM_c` (Torch Slug, `include/d/actor/d_a_e_hm.h`):
`ActionMode()`/`At_Check()` public members; damage applied inline
(`S16_SUB(health, mAtInfo.mAttackPower)`, `d_a_e_hm.cpp:1078`); collider
`mSph` (0x928).

`b_gm_class` (Armogohma, `include/d/actor/d_a_b_gm.h`): same shape as a regular
enemy — `mAction`/`mMode`/`mTimers[4]`/`mInvincibilityTimer`, many `dCcD_Sph`
colliders, `mAtInfo`, `mHitCount` — just bigger and story-gated (§6).

### 3.4 Health is NOT one semantics

> **SUPERSEDED — the E_FB ("Fire Baba") and E_GS lines below are STALE; see
> m2-design-notes.md §2/§5 for the authoritative whitelist.** E_FB is the
> Freezard (not the "Fire Baba" an earlier draft assumed), iron-ball-only
> damage, dropped from the shipped whitelist; E_GS has no damage collider,
> dropped. The shipped whitelist is `E_AI, E_HM, E_DF, E_YC, E_MD, B_TN`
> (capstone MINOR 3, review-full-glm-5.2.md).

- ~77 enemies: `health` is real HP, decremented by `cc_at_check`.
- Armos (`E_AI`): `health = 1000` is **reset every frame** in `damage_check`;
  it "dies" by `m_hitCount >= 3` (or specific AtTypes). HP is meaningless for
  it — the snapshot needs hit-count semantics.
- Torch Slug: inline HP minus power.
- Fire Baba (`E_FB`): forced `health = 0` by interaction types.
- Several specials (`E_PH`, `E_GA`, …) have no health references at all.

The whitelist adapter therefore carries a **damage/death-semantics tag**
(`hp | hitCount | special`) per type; the generic snapshot always carries
`hp/maxHp` and the adapter decides what they mean.

### 3.5 Generic `EnemyState` snapshot schema

Aligns with the provisional `EnemyState` in `00-network.md` §5, concretized:

```
enemyId    u16      // owner-assigned session id (§3.1)
type       u16      // procName (fpcNm_E_*/B_*)
pos        f32 x3   // current.pos
angle      s16      // current.angle.y (shape_angle.y implied; include both if
                    //   types like Zant need pitch/roll — v1: yaw only + flags)
speed      f32 x3   // optional, for velocity-sensitive knockback/anim
hp         u16      // health  (per §3.4 semantics tag decides meaning)
maxHp      u16      // field_0x560
anim       u32      // per-type packed: action id (u16) + anim frame (u16);
                    //   adapter maps to m_action + m_modelMorf frame
aggro      u8       // target player id (0xFF = none) — display/lock-on only
flags      u8       // dead / downed / wolf-bitten (fopEn_enemy_c::mFlags subset),
                    //   boss-phase hint
semantics  u8       // per-type tag from the adapter (hp|hitCount|special)
≈ 44–52 B/enemy
```

Per-enemy dirty flags + sequence numbers (as in `00-network.md` §6); full
snapshot every 30th packet per enemy.

---

## 4. Combat path and the validation intercept

### 4.1 How damage actually flows (verified end to end)

1. **Pair detection** — `dCcS::Move` → `cCcS::Move` sweeps registered At/Tg
   pairs; `dCcS::ChkAtTgMtrlHit` / `ChkNoHitGAtTg` apply the material table
   (`d_cc_s.cpp:911/915`).
2. **Registration** — `dCcS::SetAtTgGObjInf` (`d_cc_s.cpp:534`, virtual, public
   in `include/d/d_cc_s.h:34`): sets `AtHitPos/TgHitPos`, `TgRVec`, hit-apids,
   shield flags; calls `i_tgStts->PlusDmg(atp)`; invokes the At and Tg hit
   **callbacks**; fires `ProcAtTgHitmark`. The Tg collider's `ChkTgHit()`
   flag (`cCcD_ObjTg::ChkHit()`, RPrm bit 1) is what the enemy later polls.
3. **Enemy-side processing (next frame's execute)** — enemy polls
   `ChkTgHit()`, builds `dCcU_AtInfo` (`mpCollider = GetTgHitObj()`), and calls
   either the shared `cc_at_check` (`d_cc_uty.cpp:382`, public in
   `include/d/d_cc_uty.h:35`) or an inline handler:
   - `cc_at_check`: `at_power_check` → **`i_enemy->health -= i_AtInfo->mAttackPower`**
     (`d_cc_uty.cpp:475`); on death `mHitStatus = 2; i_enemy->health = 0`
     (480–481); hit direction from `mpActor->speed`; pause timers, hitmarks.
   - inline variants: `daE_HM_c::At_Check` (`S16_SUB(health, …)`),
     `e_ai_class::damage_check` (hit-count model).
4. **Power computation** — `at_power_get` (file-local static, per-`mPowerType`
   scaling table) + `at_power_check` (public, `d_cc_uty.h:36`) which applies
   equipment multipliers (master sword ×2, wood sword ÷2, `getSwordAtUpTime`
   ×2, wolf, iron ball 200, …) and resolves `mpActor`. `cc_pl_cut_bit_get()`
   (public, `d_cc_uty.h:34`) feeds `mHitBit` from the attacker's cut type.

### 4.2 What the in-tree mod already intercepts (reference implementation)

All in `src/d/d_cc_s.cpp` + `src/d/d_cc_uty.cpp` under `#if TARGET_PC`:

- `SetAtTgGObjInf`: `combat::filterAtTgHit(at, tg, &contactOnly)` (friendly
  fire + cut-type attribution), `combat::noteAtTgHit(...)` (frame-batched hit
  event capture), `combat::consumeSuppressPlusDmg()` (→ `contactOnly`, skips
  `PlusDmg`), `combat::popAttackCutType()`.
- `ChkNoHitGAtTg`: `combat::shouldBlockAtTgCompletely(...)` → reject the pair.
- `d_cc_uty.cpp`: `resolvedCutType()`/`pushAttackCutType`/`popAttackCutType`
  (`cc_pl_cut_bit_get`), `cutTypeForActor` (attribution).

This is exactly the shape of the network forward point — the fork proved the
hooks work at these two functions.

### 4.3 Client-side: the intent capture point

**Forward point: pre-hook on `dCcS::SetAtTgGObjInf`** (virtual member; hook by
display name `dCcS::SetAtTgGObjInf` or decorated symbol). At that point the mod
has the full pair:

- `atObjInf->GetAc()` — attacker actor (a local Link, or a projectile the mod
  owns via source tracking);
- `tgObjInf->GetAc()` — target actor (map to enemy entity id via the registry);
- `i_atObj->GetAtAtp()`, `ChkAtType(...)`, `GetAtMtrl()` — attack kind;
- `i_hitPos` — contact point (range validation on the owner).

Flow per `00-network.md` §7:

```
client: local Link hits enemy X
  → SetAtTgGObjInf pre-hook
  → compute power: build a synthetic cCcD_Obj (public setters
    SetAtp/SetType/SetAtSe) + dCcU_AtInfo, call public at_power_check()
    (equipment multipliers read player 0 = the local attacker — correct)
  → CombatIntent{ attacker playerId, target entityId, atp, atType,
                  mPowerType, mHitType, computed power, hitPos, attacker pos }
  → send (reliable, on event)
  → puppet HP is never touched locally (the freeze, §1.3)
```

No extra local suppression is needed for frozen puppets: they never reach
`cc_at_check`. The fork's `consumeSuppressPlusDmg`-style gate is only needed
for *unfrozen* local enemies that belong to a remote room owner (v2/§8.3) —
then hook `cc_at_check` too and zero `mAttackPower` for remote-owned targets
(the fork's `invincibleEnemies` setting shows the exact pattern,
`d_cc_uty.cpp:464`).

### 4.4 Owner-side: applying a validated intent

The owner simulates the enemy normally (AI runs, colliders registered). On
receiving `CombatIntent`, the owner:

1. **Validates** — entity exists, room owned, enemy alive, attacker in range
   (from `hitPos`/`attacker pos`), attack allowed (v1: friendly-fire intents
   rejected per `00-network.md` §7).
2. **Fabricates the collision result** — the engine-faithful injection:
   - set the target enemy's damage collider Tg hit flag
     (`cCcD_ObjTg::SetHit(syntheticObj)` or `OnRPrm(1)` — public), and
   - set the enemy's `m_atInfo.mpCollider` to a **synthetic attacker
     `cCcD_Obj`** carrying the intent's atp/type/se (the setters are public),
     `m_atInfo.mHitDirection` from the intent's positions, `mHitStatus = 0`,
     `mAtInfo.mpActor` = the attacker's Link **puppet on the owner's machine**
     (it exists — speed/position come from the player snapshot).
   The enemy's *own* next execute then runs its normal damage handler
   (`damage_check`/`At_Check`/`cc_at_check`), producing authentic damage,
   hitstun, knockback, sounds, and death — no reimplementation.
   The per-type bit is the collider member offset + which handler to feed,
   carried by the whitelist adapter.
3. **Broadcasts the result** — `CombatResult{ entityId, damage, newHp,
   outcome }` (reliable ack) and, on kill, `EnemyEvent(died)`; the next
   `EnemySnapshot` carries the new HP anyway.

Alternative (fallback, less faithful): call `cc_at_check` directly with a
fabricated `AtInfo` — applies HP but skips the enemy's reaction state machine;
acceptable for v1, produces wrong hitstun feel. Prefer the flag-injection
route.

**Multi-hit batching**: several clients (or a client + the owner's local Link)
may hit the same enemy in one frame. The owner queues intents per enemy and
flushes at the enemy's next execute boundary; the in-tree
`resolveSameFrameHits`/`strongestHitForVictim` model (`coop_combat.cpp`) shows
the aggregation shape (strongest reaction + total damage) to reuse.

### 4.5 Drops

On `EnemyEvent(died)`, **each machine spawns its own drops locally** (per
`network.md` §5 / `00-network.md` §7). The fork gates native drop creation
(`fopAcM_createItemFromEnemyID`/`createItemFromTable` TARGET_PC blocks in
`f_op_actor_mng.cpp:1938+`); on stock the mod hooks those same public functions
and lets the drop roll proceed only for kills it confirmed via
`EnemyEvent(died)` (a per-enemy "killed by sync" latch, since on the client the
killing blow never passes through the local damage path).

---

## 5. Room-clear / ALLDIE

### 5.1 How it works today

`daAlldie_c` (`src/d/actor/d_a_alldie.cpp`): `actionCheck` polls
`fopAcM_myRoomSearchEnemy(roomNo)` — a layer scan of group-2 actors (plus the
"grabbed enemy" special case). Empty → `ACT_TIMER` (65 frames) → on-switch →
optional event. The fork adds `enemy::roomClearBlocked()` (pending clones /
waves) inside `coopRoomStillHasEnemies` (a `static` file-local helper — stock
doesn't have it; a mod hooks `daAlldie_c::actionCheck`/`actionTimer` instead,
both public members).

### 5.2 Network model

- The **owner's** room-clear is authoritative: its own enemies die → its own
  ALLDIE fires → doors/events happen on the owner.
- On the **client**, dead puppets are **deleted on `EnemyEvent(died)`**
  (`fopAcM_delete`), so the client's own ALLDIE scan empties at the same
  moment the owner's does, and the client's door opens locally — a world change
  that rides the enemy channel, exactly as `network.md` §5 anticipates.
- **Edge cases that break the mirror**:
  - enemies that linger as corpses after death (they stay group-2 → client
    scan stays non-empty even though the owner's is clear);
  - dynamic spawns the client lacks (client scan empty while the owner still
    fights → client door opens **early**).
  Fix: the owner sends a per-room `EnemyEvent(roomClear)` (reliable, on
  change); the client's `daAlldie_c` hook holds the `ACT_CHECK → ACT_TIMER`
  transition until that bit arrives. This is the network counterpart of
  `roomClearBlocked()`, and the fork's hook location
  (`coopRoomStillHasEnemies` in `d_a_alldie.cpp`) is the reference.

### 5.3 Consistency requirement

Same enemy set per room across machines — either guaranteed by construction
(stage-placed enemies exist on every machine; §3.1) or via `EnemyEvent(spawn)`
for dynamic ones — **or** the `roomClear` bit makes the mirror unnecessary.
v1 (co-located, stage-only enemies) can run on puppet-deletion alone; v2
(waves, §8.3) needs the bit.

---

## 6. Bosses

Bosses are structurally ordinary enemies (this is the "bosses are just
enemies" claim, verified):

- All 21 `B_*` profiles extend `fopEn_enemy_c` (e.g. `b_gm_class`,
  `include/d/actor/d_a_b_gm.h`); 10 of 21 use the shared `cc_at_check`
  (incl. `B_GND`, `B_GM`, `B_BQ`, `B_TN`, `B_MGN`, `B_OH`); the rest
  (`B_GO`, `B_DS`-adjacent, `B_ZANT*` variants…) handle damage in-file.
- Death is the same `health <= 0` base check plus
  `dComIfGs_onSwitch(sw, roomNo)` (e.g. `d_a_b_bq.cpp:504/899/2153`) — the
  story gate is a room switch, nothing more.
- Spawn is story-gated by switches/events — on the owner's machine only.

So the boss policy from `network.md` §7 composes with the enemy plumbing:

- **Existence (union bit)**: the client sends "needs encounter X" in its
  room-entry message; the room owner spawns the boss iff *any present player's*
  story needs it. Clients without the boss in their stage receive
  `EnemyEvent(spawn, type=B_*, params, pos)` and create a local boss puppet
  (the fork's `spawnClone` play-scene-layer pattern). Fallback v1: boss exists
  iff the owner's story needs it.
- **Death (per-player flag)**: `EnemyEvent(died)` carries the boss death switch
  / flag mask; each client grants it **to its own local save**
  (`dComIfGs_onSwitch`, per-player `save::`), so a cleared player can help a
  friend without breaking either save.
- The boss HP bar and boss camera read the local puppet's snapshotted HP —
  no extra sync needed.

---

## 7. Per-type handling: the whitelist

### 7.1 Coverage measurement (verified)

Of 117 enemy/boss actor files (`src/d/actor/d_a_[eb]_*.cpp`):

- **77 use the shared `cc_at_check`** — these get the generic power/damage
  path; for puppets the freeze handles them, and on the owner the 
  flag-injection route reuses their normal handler.
- **40 do not** — 11 boss files (`B_BH, B_DR, B_DRE, B_GG, B_GO, B_GOS, B_OH2,
  B_YO_ICE, B_ZANT_MAGIC, B_ZANT_MOBILE, B_ZANT_SIMA`) and 29 enemy files:
  `E_AI, E_ARROW, E_BEE, E_BI_LEAF, E_BUG, E_CR_EGG, E_DB_LEAF, E_DF, E_FB,
  E_FK, E_GA, E_GS, E_HB, E_HB_LEAF, E_HM, E_HZELDA, E_MD, E_MK_BO, E_PH,
  E_PM, E_ST_LINE, E_TH_BALL, E_TK_BALL, E_WARPAPPEAR, E_YC, E_YD, E_YD_LEAF,
  E_YM_TAG, E_ZS`.

Most of the 29 are minions/sub-objects (leaves, balls, tags, eggs, arrows,
bees) that rarely matter as standalone threats; the whitelist starts with the
real room-fodder threats among them: **`E_AI` (Armos — Gate G's first entry),
`E_HM` (Torch Slug), `E_FB` (Fire Baba), `E_DF`, `E_GS`, `E_YC`, `E_MD`**,
plus any `B_*` boss a dungeon needs.

> **SUPERSEDED — see m2-design-notes.md §2/§5.** `E_FB` (Freezard) and `E_GS`
> are DROPPED from the shipped whitelist (`E_FB`: iron-ball-only damage, its
> handler casts `GetTgHitAc()` to `daObjCarry_c*` and reads slot 0 — unsafe
> for synthetic injection; `E_GS`: no damage collider). The shipped whitelist
> is `E_AI, E_HM, E_DF, E_YC, E_MD, B_TN` (capstone MINOR 3).

### 7.2 Adapter extension (from `EnemyAdapter`)

```cpp
enum class DamageSemantics : u8 { Hp, HitCount, Special };

struct NetEnemyAdapter {           // extends the fork's EnemyAdapter
    s16 procName;
    // generic drive metadata (offsets verified per type from its header):
    u32 modelMorfOffset;           // mDoExt_McaMorfSO* — anim frame
    u32 actionFieldOffset;         // m_action / mMode
    u32 animFieldOffset;           // m_anm / anim id
    u32 atInfoOffset;              // dCcU_AtInfo member
    std::vector<u32> colliderOffsets;  // damage colliders to re-register + flag-inject
    DamageSemantics semantics;
    // per-type callbacks (call public members via game headers):
    void (*refreshColliders)(fopAc_ac_c*);   // e.g. &e_ai_class::setCcCylinder
    void (*driveModel)(fopAc_ac_c*, const EnemyState&); // setBaseMtx + anim frame
    bool (*applyIntent)(fopAc_ac_c*, const CombatIntent&); // owner-side hit injection
};
```

The in-tree registry (`registerAdapter`/`findAdapter`) is directly reusable.
Per-type work is a table row + a few public-member calls, not a fork.

---

## 8. Phased implementation plan

### Phase 0 — Scaffolding (pure mod, no transport yet)

- Entity registry: owner-side `(roomNo, setID, procName)` → enemyId; client-side
  enemyId → `fopAc_ac_c*`; dynamic-spawn id counter.
- Hooks installed: `fopAc_Execute` (freeze/apply), `fopAcM_cullingCheck`
  (puppet-friendly), `dCcS::SetAtTgGObjInf` (intent capture), `daAlldie_c`
  action hooks (room-clear gate).
- `EnemyState` serializer + per-enemy dirty tracking; loopback test: host feeds
  snapshots to its own "client" side and verifies puppet parity in split-screen
  (the fork's multi-viewport render already allows side-by-side comparison).

### Phase 1 — Co-located v1 (host sims its own room; everyone in one room)

- Host: normal sim; stage-placed enemies registered; snapshots 30 Hz +
  `EnemyEvent(died)` on kills.
- Clients: freeze + drive puppets (§1.5); combat intents → host; host validates
  + injects the hit (§4.4); results/drops per §4.5; room-clear via puppet
  deletion (plus the `roomClear` bit if corpse edge cases show up).
- Whitelist: `E_AI` first (Gate G), then `E_HM`, `E_FB`, one `B_*` boss.
- Gate: puppet parity in split-screen loopback before any wire traffic.

### Phase 2 — Dynamic spawns & waves

- `EnemyEvent(spawn/die)` for owner-triggered waves, summoned minions, split
  enemies (Chu); client creation via the play-scene-layer `fopAcM_create`
  pattern; per-room `roomClear` bit becomes mandatory.
- Adapter growth for each newly whitelisted type; snapshot delta compression.

### Phase 3 — Room ownership (distributed authority)

- Ownership per `network.md` §6: first-in-room owner, sticky transfer on
  leave/disconnect; intents route to the *room's* owner (message destination,
  per `00-network.md` §2); owner takeover re-sims per its own story (dead
  enemies may resurrect — accepted).
- Entity id stability across ownership transfer (map transfer, not renumber).

### Phase 4 — Boss policy

- Union existence bit on room entry; owner spawns when any present player needs
  the encounter; `EnemyEvent(died, flagMask)` → per-player local flag grant.
- Bosses as enemies end to end; boss-camera/HP-bar verified against puppets.

---

## 9. Open risks / integration notes

- **Stock AI only targets player 0.** Enemy AI uses the *inline*
  `fopAcM_searchPlayerAngleY/Distance` helpers (`f_op_actor_mng.h`), which on
  stock resolve `dComIfGp_getPlayer(0)` — the host's own Link. The fork solved
  this with `fopAcM_getContextPlayer()` + `ScopedContext`; a pure mod cannot
  hook inlines. Consequence: on the host, enemies aggro the host's Link only
  (clients' damage still lands via `CombatIntent` — the enemy doesn't need to
  "see" the client for intents). If host-only aggro is unacceptable, the
  fallback is per-type AI hooks — explicitly out of v1 scope.
- **Ghost side effects** are impossible for frozen puppets (execute skipped),
  which is the main argument for the freeze over "run AI + overwrite".
- **Latency feel**: LAN RTT ≈ 0–2 frames; the collision pass runs in the draw
  phase, so intent→owner-apply is ~1 frame regardless of transport order.
  No prediction in v1 (`00-network.md` §12).
- **Culling**: stale `cullMtx` on frozen puppets ⇒ hook `fopAcM_cullingCheck`
  (see §1.3).
- **`fopAc_Execute` skip vs. frame-interpolation**: the fork's
  `dusk::frame_interp` replays draws; puppet state applied per snapshot must
  not fight the interpolator (set `old = current` on apply; revisit when the
  player-puppet investigation 02 lands).
- **Entity id stability** across room-owner transfers is the one protocol-level
  constraint worth pinning in `00-network.md` (it already lists it as an open
  point, §11.3).
