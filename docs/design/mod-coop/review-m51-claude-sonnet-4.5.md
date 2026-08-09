# Adversarial review — M5.1 SHRED (the M5 pivot lands)

**Subject:** `723f7f42ea..HEAD` (7 commits) on `net-coop` — the M5.1 shred of the entire
M2/M3/M4 owner-authoritative enemy/combat authority stack, landing protocol v7 + puppets/time/
weather only.
**Reviewer:** adversarial pass. Range `723f7f42ea..HEAD`: protocol v7 · session strip · enemy
shred · combat+context delete · vanilla revert · dead-export drop · selftest v7.
**Method:** grep-driven §2-table audit over the whole tree + forced rebuild (touched TUs) +
selftest ran twice + full-game link. Read-only — no source modified, nothing committed (only this
file added).

---

## 1. VERDICT

**The shred is COMPLETE and regression-clean. HEAD builds green, selftest-green (242 checks,
ran twice → PASS both), and is a sound base for M5.2 (ghost sender).**

- Every artifact named in the 05-ghosts.md §2 table is gone from **code** — the full-tree grep
  finds each symbol only in (a) the §2 table itself, (b) archived milestone-review docs, or
  (c) selftest rows that *assert the wire death*. **Zero surviving source reference.**
- The vanilla reverts (`d_cc_s.cpp`, `d_a_alldie.cpp`, `d_a_e_yc.cpp`, `d_a_e_ai.cpp`,
  `f_op_actor.cpp`, `f_op_actor_mng.h`, `d_a_player.h`) are **byte-identical to pre-coop vanilla**
  (checked against `bb235ed585^`), with one trivial cosmetic exception (below).
- `d_cc_uty.cpp` and `d_attention.cpp` show **ZERO diff** in the M5.1 range, as required.
- Protocol v7 is internally consistent: type count 13, `WireSize`/`ChannelFor`/determinism over
  13, 34-B ghost / 4-B death rows assert, dead wire id 16 + old combat id 11 are size-rejected,
  `senderId` is on the wire (M5.2 registry keying works), and the stub ships **zero** ghost
  traffic.
- Forced rebuild of the touched TUs + the full `dusklight` link are green (only pre-existing,
  unrelated JSystem/C++17 deprecation warnings). No dangling includes, no header-inline referring
  to a deleted symbol, no config/cvar pointing at a deleted module.

**BLOCKER: none.**
**MAJOR: one (documentation-only) — TESTING.md still describes the dead authority model with no
supersede banner.** The expected "MUST-FIX before M5.2: none" does **not** hold for docs: a
tester following TESTING.md §1/§3 will be told to verify features M5.1 deleted (details below).
**MINOR:** three (cosmetic alldie hoist; net-off selftest row is session-level by construction;
historical design docs retained without TESTING.md's banner).

The headless agent's 100% grep-driven shred-completeness claim **holds** — I re-derived it
independently and found no code miss. Its only seam is the docs surface (TESTING.md), which the
shred intent ("leave nothing dangling") did not fully cover.

---

## 2. VERIFIED-OK — the big claims A–F, with evidence

### A. Shred completeness (grep-driven §2 checklist) — **CONFIRMED, no code misses**

Ran `grep -rn <symbol>` over the whole tree for every §2 row. Every deleted symbol now appears in
**no** `.cpp`/`.h`/`.inc` outside comments/selftest. Representative evidence:

| Symbol | Surviving code refs |
|---|---|
| `RoomOwnershipMsg` / `RoomOwnershipTable` / `OwnerOf` / `ownership_` / `playerRoom_` / `UpdatePlayerRoom` / `SendToAllInRoom` / `RouteCombatIntent` | none (only protocol.h **version-history** comment :63/:78-83 + §2 table + archived review-m4/*.md) |
| `amIRoomOwner` / `remoteInRoom` / `puppetActorFor` / `resolveNearestPlayer` | none — dead coop.h exports removed |
| `noteAtTgHit` / `applyInjectedHit` / `SetTgHitSynthetic` | none (`SetTgHitSynthetic` appears only in a coop_enemy.cpp:25 comment explaining why `g_entries` stays a node-based `std::map` — not a reference) |
| `clientRoomCleared` / `hostRoomCleared` / `g_roomClear` / `g_roomHadEnemies` | none — `d_a_alldie.cpp` is vanilla (`mAction=ACT_TIMER; mTimer=65` restored) |
| `ScopedEnemyTarget` / `currentTargetPlayer` / `hostOnExecuted` / `hostNeedsContext` | none — `f_op_actor.cpp` :286/:335/:377 blocks removed, `fopAcM_getContextPlayer` shim deleted |
| `SpawnDropAndBeat` / `GrantLocalSwitch` / `kAdapters` / `isWhitelistedType` | none |
| `puppetExecute` | **only** as the surviving **M1 puppet-apply** path (`coop.h:66`, `coop.cpp:1160`, `d_a_alink.cpp:17964`, `isPuppet`-gated). The **enemy-freeze** `puppetExecute` pre-guard in `f_op_actor.cpp` that §2 names is gone. No collision — different function, correctly kept. |

Files/headers: `coop_combat.h/.cpp`, `coop_context.h/.cpp` are deleted; **no** `.cpp`/`.h`/`.inc`
in the tree includes them (grep empty); `files.cmake` :1421/:1437 removed all four from the build.
No cvar/config entry references a deleted module.

### B. Vanilla-equivalence contract — **CONFIRMED**

`git diff bb235ed585^..HEAD` (parent of the coop-attach commit, i.e. the pre-coop vanilla
baseline) for the 7 reverted files is **empty** except:

- `src/d/actor/d_a_alldie.cpp` — a single cosmetic hoist `const s8 roomNo = fopAcM_GetRoomNo(this);`
  moved above the `if`. Behavior-neutral, honestly not a degraded shim (see MINOR B).
- `f_op_actor.cpp`, `d_cc_s.cpp`, `d_a_e_yc.cpp` (6 sites), `d_a_e_ai.cpp` (3 sites),
  `f_op_actor_mng.h` (10 wrappers back to `dComIfGp_getPlayer(0)`; shim deleted), `d_a_player.h`
  (both `daPy_get*` back to vanilla bodies): **byte-identical** to pre-coop vanilla.

Remaining `#if TARGET_PC` blocks in the surviving vanilla TUs are all **documented survivors**,
none enemy-authority: `d_cc_s.cpp:12/:773` (collisionView debug), `d_cc_uty.cpp:16/440-460`
(`invincibleEnemies` cheat + achievements — fork features, explicitly NOT shredded per gF8),
`d_attention.cpp:15/655/1441` (655 = M1 `isPuppet`-style lock-on exclusion, survives per design).
`f_op_actor.cpp`, `d_a_alldie.cpp`, `d_a_e_yc.cpp`, `d_a_e_ai.cpp` now contain **no** TARGET_PC at
all.

`d_cc_uty.cpp` and `d_attention.cpp`: `git diff 723f7f42ea..HEAD` on both is **empty** — zero diff.
✓

### C. Protocol v7 — **CONFIRMED, deviation #1 resolves clean**

- **Type count 16 → 13.** Old enum had `CombatIntent=11, CombatResult=12, RoomOwnership=16`
  (verified in `723f7f42ea:protocol.h:111-128`); new enum compacted to `1..13`, `TimeSync=11,
  TimeEvent=12, WeatherChange=13` (`protocol.h:141-155`). `PayloadUnion` (13 members),
  `WireSize` over all 13, `ChannelFor` (PlayerState/GhostSnapshot/TimeSync unreliable, rest
  reliable) all consistent.
- **Dead wire ids rejected:** `DeserializeMessage` rejects `typeRaw` outside 1..13
  (`protocol.cpp:262`), so RoomOwnership (16) is dropped; a stale CombatIntent frame at id 11
  (now TimeSync, 8 B) with the old 42-B payload is rejected by exact-size check
  (`protocol.cpp:268`). Selftest pins both (`RunProtocolChecks` + `RunV7NetOffRegression`).
- **Version-history comment rewritten:** the stale "M5 bumps to v7 (adds spawn params, horse
  channel)" promise and the M4.5 "wire-vs-drop" notes are gone. The new block
  (`protocol.h:76-84`) accurately records v7 as a **breaking deletion**, resolves the wire-vs-drop
  notes as **DROPPED**, and states the `PlayerEventMsg.scene` semantic re-parenting to the
  receive-side puppet gate. No "spawn params"/"horse channel" text survives anywhere in header or
  transport comment (transport.h comment updated too).
- **Ghost wire 34 B / EnemyEvent 4 B assert correctly.** `GhostSnapshotMsg`
  (`protocol.h:379-390`) = senderId+flags+entityId+type+angle+animFrame+pos+speed =
  1+1+2+2+2+2+12+12 = **34 B** (`protocol.cpp:177`), exactly the design §4.2 figure (the
  deepseek B1 35/36-B objection was against the old v6 `u32 anim`; v7's `u16 animFrame` makes
  34 B correct). `EnemyEventMsg` (`protocol.h:395`) = senderId+eventId+entityId = **4 B**.
- **Deviation #1 (the stub's §4.2 field set): RESOLVED.** `senderId` is present (`protocol.h:380`)
  — so M5.2's `(senderId, entityId)` registry keying works and the host self-echo drop
  (`senderId == selfId()`) is implementable. `animFrame` (frame-only) matches the §4.2/§4.3
  frame-only fidelity decision and d-B2's honest "action ids never on the wire". **M5.1 ships ZERO
  ghost traffic**: the only serializers/receivers exist; no game code calls
  `SendGameMessage(GhostSnapshot/EnemyEvent, ...)` (coop.cpp sender handles only
  PlayerState/PlayerEvent; coop_enemy `onGameFrame` only resets tables). `GhostSnapshotMsg` is
  wire-ready, not wired.

### D. Selftest integrity — **CONFIRMED**

- Dropped suites: `RunM2RelayPolicyCheck`, `RunM4OwnershipTableCheck`, `RunM4RoomRoutingCheck`,
  `RunM4EntityStabilityCheck` removed; M4 ownership parts dropped from `RunM4WorldStageCheck`
  (grep of surviving body shows no `owner`/`RoomOwnership`/`playerRoom`). Kept:
  `RunProtocolChecks`, `RunDeterminismChecks` (extended to 13 types), `RunHandshakeDemo`,
  `RunGameMessageDemo`, `RunM3TimeWeatherCheck`, `RunM35TimeWeatherFixCheck`,
  `RunM46SessionRestartCheck`, `RunM4WorldStageCheck`, `RunM4DiscoveryCheck`, plus new
  `RunV7NetOffRegression` (+ validation/ring/clock/generation/dup-join rows).
- **Count:** `grep -cE '^\s*ok:'` = **242**. Ran twice after a forced rebuild → **PASS: all
  checks succeeded**, exit 0, both runs. (Declared 242 in the commit message matches; the v7
  GhostSnapshot 34-B + EnemyEvent 4-B rows run.)
- **Net-off regression row actually asserts something** (`RunV7NetOffRegression`): version=7,
  ids compact 1..13, an idle never-started `Session` refuses `SendGameMessage(GhostSnapshot/
  EnemyEvent)` (zero ghost sends at the session boundary), and `WireSize(id 16)==0` /
  `WireSize(id 11)==8` (dead combat id). The "zero registry entries / `myRoomSearchEnemy`
  bit-identical" halves are **not** literally asserted — they hold **by construction**: the enemy
  stub's `onGameFrame` early-returns when `!sessionActive()` (clearing entries once,
  `coop_enemy.cpp:148-157`) and every remaining TARGET_PC block is `isPuppet`/session-gated with
  `net.enabled=false` dead-ending the session pump. Honest session-level row + game-side
  by-construction, matching the design's d-m8 wording. (Observation, not a defect.)

### E. The 🤔 dangling-dependency risk — **CONFIRMED SAFE**

- Full `dusklight` link after touched-TU force rebuild: **green** (33903 exports, no unresolved
  externs).
- **Header-inline trap checked:** the only `daPy_getPlayerActorClass`/`daPy_getLinkPlayerActorClass`
  usages across the tree are vanilla callers of the restored vanilla inline functions (both now
  body `dComIfGp_getPlayer(0)` / `dComIfGp_getLinkPlayer()`, no `dusk::coop` dependency). The
  deleted `fopAcM_getContextPlayer` inline (which referenced `currentTargetPlayer`) is gone; a
  whole-tree grep for `currentTargetPlayer` / `fopAcM_getContextPlayer` / `coop::combat` /
  `coop::context` in `src`+`include` returns nothing. No header-only inline references a deleted
  function.
- No surviving subsystem (formation, time/weather `coop_time`, worldStage carry, config/cvars)
  references a deleted symbol. The `dusk::coop::enemy::` namespace survives only as the stub's
  `onGameFrame`/`shutdown` (called from `coop.cpp:1342/:1373`) + the kept `coop_entity_logic.h`
  id scheme (StageEntityId/DynamicEntityId/kInvalidEnemyId/kDynamicCounterMask all intact).
- `worldStage_` carry in session references only surviving fields (stage/time/weather) — no
  removed session fields. All coop.h exports have live definitions (link green).

### F. Docs/leftover surface — **ONE REAL CONTRADICTION (MAJOR), plus acceptable history**

- **`TESTING.md` is the loose end.** It has **no** supersede banner and its §1 selftest-coverage
  text (lines 60-66) and §3 in-game matrix (lines 128, 131, 133) **describe the dead authority
  model as the current procedure**:
  - :60 "incl. the M4 RoomOwnership message"
  - :63-64 "CombatIntent routed to the ROOM owner; EnemySnapshot room-scoped; CombatResult…"
  - :66 "M4 room ownership …"
  - :128 M2: "client's hits kill it (host applies); drops spawn on BOTH; room-clear doors open together"
  - :133 M4 ownership: "combat from either side lands via the room-owner route; the owner leaving transfers the room"
  - status banner (line ~6): "M0–M4 + M4.5 + M4.6 capstone pass landed" — no M5.1 line.
  A tester following this expects checks/features the v7 selftest deliberately removed. This is
  the one doc that would genuinely mislead the M5.2 developer — **MUST-FIX (docs)**.
- `00-network.md` §6 (room ownership) / §5 (EnemySnapshot/CombatIntent tables), `implementation-plan.md`
  M4 status and `03-enemies.md` still describe the deleted stack, but 05-ghosts.md §1
  **explicitly supersedes** all three for M5+. Archived history — acceptable, not must-fix.
- The two `review-m5-design-*.md` files and `05-ghosts.md` v2 are current and consistent with HEAD;
  no contradiction.

---

## 3. MUST-FIX before M5.2

Expected "none" — the code is clean. **One documentation root-cause remains:**

1. **TESTING.md (MAJOR, docs).** Add an M5.1/M5.2 status line + supersede note and trim/replace
   the §1 selftest-coverage description and §3 M2/M4 rows that still describe the deleted
   owner-authoritative/room-ownership/combat model (`TESTING.md:60-66,128,131,133`). The §7
   ghost-probe list in 05-ghosts.md is the correct replacement for the M2/M4 in-game rows. Without
   this, the M5.2 acceptance (and any tester) reads instructions for features that no longer exist.

## 4. NICE-TO-HAVE (no action required for M5.2)

- **MINOR (cosmetic):** `d_a_alldie.cpp` hoists `const s8 roomNo` above the `if` — a one-line
  divergence from pre-coop vanilla that is behavior-neutral (not a shim). Optionally collapse to
  the exact vanilla `if (fopAcM_myRoomSearchEnemy(fopAcM_GetRoomNo(this)) == NULL)` in a future
  commit if byte-equality is ever asserted mechanically.
- **MINOR (honest row, already acceptable):** the net-off regression could additionally assert
  `g_receive`/enemy-registry emptiness on an off-session `onGameFrame` if M5.2 ever wires an
  off-session path; today it's honest session-level + by-construction game-side.
- **Minor (optional hardening for M5.2):** with `senderId` now on the wire, add a `WireSize`/
  round-trip row that also round-trips a host self-echo `senderId == selfId()` GhostSnapshot
  (the deserialize path already accepts it; the drop is M5.2 sender-side). Not required — the
  existing 34-B round-trip row covers layout.

## 5. Bottom line for M5.2

HEAD is a clean, fully-vanilla-reverted base with the ghost wire types laid out exactly as
05-ghosts.md §4.2 specifies, `senderId` present, and zero ghost traffic emitted — the M5.2
sender lands on `coop_enemy`'s kept entity-id scheme + the stub `onGameFrame` scan-point seam
with no leftover authority surface to untangle. Approve as the M5.1 base; fix TESTING.md before
handing it to the next milestone.

*Review artifacts (no source modified): full-tree greps, `git diff` against `bb235ed585^` and
pre-coop vanilla, forced rebuild of touched TUs, `dusk_net_selftest` ×2 (242 ok, PASS), full
`dusklight` link green.*
