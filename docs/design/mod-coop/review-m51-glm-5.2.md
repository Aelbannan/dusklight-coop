# Adversarial M5.1 SHRED review — `05-ghosts.md` §2/§3/§5 + the §2 attachment checklist

**Subject:** the M5.1 shred — `git diff 723f7f42ea..HEAD` (7 commits: protocol v7, session strip,
enemy shred, combat+context delete, vanilla revert, dead-export drop, selftest v7). Branch `net-coop`.
**Reviewer:** glm-5.2 (z-ai/glm-5.2). **Mode:** read-only — no source modified, no commit, only this file.
**Method:** re-derived every §2-table row by grep over the whole tree; read every surviving file; forced
rebuild of both the `dusk_net_selftest` target (5 net TUs) and the full `dusklight` game target (1188 TUs
incl. the coop + vanilla-reverted files); ran the selftest twice.

---

## 0. VERDICT

**Is the shred complete and regression-clean? YES — for the CODE surface.** Every row of the §2 table is
gone from code, no dangling reference to any deleted artifact survives in any `.cpp`/`.h`/build file, the
full game links green, and the selftest is green (242 checks, two runs, zero FAIL). The headless agent's
"the tree builds with PUPPETS + TIME/WEATHER ONLY" claim is **100% accurate for code** — I re-derived the
full grep checklist (§A) and caught zero misses in code. The surviving `puppetExecute` in `coop.h:66` /
`coop.cpp` is the PLAYER-puppet frozen-pose function (`int puppetExecute(daAlink_c*)`), a different symbol
from the deleted enemy pre-guard of the same name in `f_op_actor.cpp`; the enemy pre-guard is confirmed
deleted in the diff. The surviving `RelayPolicy`/`PolicyFor` in `session.cpp:36-58` is the STRIPPED enum
(`None`/`Star` only — `RoomScoped` is gone); the grep pattern matched the enum name, not the dead branch.

**Is HEAD buildable, selftest-green, and a sound base for M5.2? YES.** The protocol v7 wire
(`GhostSnapshotMsg` 34 B with `senderId`, `EnemyEventMsg` 4 B, 13 types 1..13) matches `05-ghosts.md §4.2`
byte-for-byte; `senderId` is present so M5.2's `(senderId, entityId)` registry keying works; the dead wire
ids (16 = `RoomOwnership`, 11 = old `CombatIntent`@42 B) are size/range-rejected; the stub ships the §4.2
field set with **zero ghost traffic** (no `OnGameMessage` branch for `GhostSnapshot`/`EnemyEvent`, no sender
in `coop_enemy::onGameFrame`). The M5.2 ghost sender lands on a clean, correct seam.

**Is there a catch? One — and it's DOCS, not code.** The authoritative M5 spec (`05-ghosts.md`) is
committed, correct, and self-consistent. But the docs it claims to supersede (`00-network.md` §5/§7,
`implementation-plan.md` M4/M5 status, `TESTING.md` status) still describe the dead authority model
(`CombatIntent`/`CombatResult`/`RoomOwnership` on the wire, `EnemySnapshot` not `GhostSnapshot`, "room
owner→all" routing) as the CURRENT architecture, with **no supersede banner** pointing back to
`05-ghosts.md`. `00-network.md §5` is the worst case — its message table is labeled "normative" and lists
exactly the three deleted messages as live. This is the headless agent's one overstatement: "nothing
dangling" is true for code, not for docs. See §F / MINOR F1–F3. None of it blocks M5.2 (the M5.2 dev is
directed to `05-ghosts.md`, which is correct), but a cross-reference into `00-network.md §5` will mislead.

**No BLOCKERS. No MAJOR code findings.** One MAJOR doc finding (F1), four MINOR (one code-comment, four
doc). MUST-FIX before M5.2 (code): **none**.

---

## 1. BLOCKER / MAJOR / MINOR (file:line + fix)

### BLOCKER
*(none)*

### MAJOR
*(none in code)*

### MAJOR F1 — `00-network.md §5` "normative" message table + §7 describe the DEAD v6 model as current
**Files:** `docs/design/mod-coop/00-network.md:148-158` (§5 message table), `:280-296` (§7 "Enemy authority
& combat"), `:320` (§6 "room ownership per `network.md` §6 — ownership transfers on").
**Problem:** `00-network.md`'s status line (`:3`) says "Design — authored by main session" with no
supersede banner. Its §5 message table — explicitly labeled "the hand-written serializers in
`src/dusk/net/protocol.cpp` are normative" — lists `EnemySnapshot` (`:151`), `CombatIntent` (`:153`),
`CombatResult` (`:154`), `RoomOwnership` (`:158`) as the live v6 message set. §7 (`:280`) draws the full
`client(attacker) → host/sim owner → CombatIntent → CombatResult` combat-routing diagram as the current
design. **All of this is the deleted authority model.** `05-ghosts.md:7-8` declares it supersedes
"`00-network.md` §6 (room ownership)" and says "the transport/session/protocol core (`00-network.md`
§1–§5) ... is unchanged" — but §5's message TABLE did change (3 types deleted, `EnemySnapshot`→`GhostSnapshot`,
v6→v7), so `05-ghosts.md`'s own "§1–§5 unchanged" is loose phrasing for "the envelope/serializer
mechanism is unchanged"; the §5 table itself is stale. An M5.2 dev cross-referencing the "normative" §5
table would read `CombatIntent`/`EnemySnapshot` as live and be actively misled.
**Fix (one-line, doc-only):** add a supersede banner under `00-network.md §5`'s heading: "v7 (M5.1,
`05-ghosts.md §3`): `CombatIntent`/`CombatResult`/`RoomOwnership` deleted, `EnemySnapshot`→`GhostSnapshot`
(34 B); see `protocol.h` for the normative 13-type set. §7's combat routing is DROPPED — see `05-ghosts.md`
§1/§2." (Same for §7.) OR update the table to the v7 13-type set. No code change.

### MINOR
1. **`coop_enemy.cpp:25` — comment names a deleted function.** `// stable across inserts (the old
   SetTgHitSynthetic pointer discipline)` — historical context for why `g_entries` is a node-based
   `std::map`, not a dangling code reference. Survives because it explains the map choice the M5.2 sender
   inherits. **Fix (optional):** reword to "the old M2 synth-collider pointer discipline" without naming
   the deleted symbol, or leave (it is accurate history). No code impact.

2. **`protocol.h:163` + `00-network.md:228` — "horse entity channel is M5"** is now wrong on two counts:
   M5 is the ghost pivot (not the horse channel), and `05-ghosts.md §8` puts horse ghosts + mount sync at
   **M6**. `PlayerEventId::Mount`/`Dismount`/`Respawn` are correctly `(reserved)`; only the "M5" tag is
   stale (pre-existing, not introduced by the shred). **Fix (optional):** `s/M5/M6/` in both comments.

3. **`implementation-plan.md:173-262` — M4 "IMPLEMENTED", M5 "Future", no M5.1 record, no supersede
   banner.** The M4 status block (`:175` "IMPLEMENTED (M4 complete...)"), the room-ownership section
   (`:221`), and `### M5 — Future` (`:258` "Dynamic waves... horse entity channel... (Room ownership
   moved to M4.)") all describe the pre-pivot plan; the M5.1 shred (the pivot itself) is not recorded,
   and there is no back-pointer to `05-ghosts.md`. `05-ghosts.md:7-8` says it supersedes
   `implementation-plan.md` M2/M3/M4 — but the plan doesn't say so. **Fix (doc-only):** a one-line
   banner under `### M5 — Future` → "SUPERSEDED by `05-ghosts.md` (M5 pivot): the M2/M3/M4
   enemy/combat/ownership direction below is DROPPED; M5.1 shredded it, M5.2+ ships parallel-worlds
   ghosts. Kept for history." + update the M4 status to "DROPPED in M5.1 (see 05-ghosts.md)".

4. **`TESTING.md:5-12` (status block) + `:60`, `:66`, `:131-133` — "M0–M4 landed", no M5.1; M4 ownership
   test rows describe the dead model as current.** The status block says "M0–M4 + M4.5 + M4.6 landed"
   with no M5.1 mention; `:60` references "the M4 RoomOwnership message" as a live wire-determinism test;
   `:131-133`'s "M4.5 join (stay-put)" + "M4 ownership" rows say "each room's enemies sim on that room's
   owner... combat from either side lands via the room-owner route" — the dead authority model. The
   actual selftest CODE is correct (v7, 242 checks, the `RunV7NetOffRegression` row); only the TESTING.md
   PROSE is stale. **Fix (doc-only):** add an M5.1 line to the status block ("M5.1 SHRED landed:
   `05-ghosts.md` — authority stack deleted, protocol v7, 242-check selftest green") and mark the M4
   ownership rows "DROPPED in M5.1 (see 05-ghosts.md)".

---

## 2. VERIFIED-OK (claims A–F, re-derived with real evidence)

### A. Shred completeness — the grep-driven checklist (RE-DERIVED, 100% for code)
Ran `grep -rni` for every §2-table artifact over the whole tree (`*.cpp`/`*.h`/`*.cmake`/`CMakeLists.txt`,
excluding `.git`/`build`). **Every one is gone from code:**

| Artifact | code hits | note |
|---|---|---|
| `RoomOwnershipMsg`/`RoomOwnershipTable`/`OwnerOf`/`ownership_`/`playerRoom_`/`UpdatePlayerRoom`/`SendToAllInRoom`/`RouteCombatIntent` | 0 | ✓ |
| `amIRoomOwner`/`remoteInRoom`/`puppetActorFor`/`resolveNearestPlayer` | 0 | ✓ (the surviving `RemoteInOurRoom` in `coop.cpp` is a different symbol) |
| `noteAtTgHit`/`clientRoomCleared`/`hostRoomCleared`/`g_roomClear`/`g_roomHadEnemies` | 0 | ✓ |
| `SetTgHitSynthetic`/`applyInjectedHit` | 0 code; 1 comment (`coop_enemy.cpp:25`, historical) | see MINOR 1 |
| `CombatIntent`/`CombatResult` | 0 code; comments only (version-history in `protocol.h:78-79`, `protocol.cpp:262`, selftest `:384/:398/:432`) | the comments describe the deletion — correct |
| `ScopedEnemyTarget`/`currentTargetPlayer`/`hostOnExecuted`/`hostNeedsContext`/`puppetExecute`(enemy)/`SpawnDropAndBeat`/`GrantLocalSwitch`/`kAdapters`/`isWhitelistedType` | 0 | ✓ |
| `RelayPolicy`/`PolicyFor` | survives STRIPPED (`session.cpp:36-58`, `None`/`Star` only; `RoomScoped` gone) | ✓ — `ForwardGameMessage` (`:549-555`) is star-only |

**Deleted module files + bridge headers:** `coop_combat.cpp`/`coop_combat.h`/`coop_context.cpp`/
`coop_context.h` are gone (confirmed: `git diff --stat` shows them as deletions; `ls` finds nothing).
**No build file references them:** `grep -n 'coop_combat\|coop_context' files.cmake` → 0 hits; the
`files.cmake:1423-1441` coop block lists only `coop.h`/`coop_enemy.h`/`coop_time.h` + their `.cpp`.
**No source includes the deleted headers:** `grep -rn 'coop_combat\.h\|coop_context\.h'` over `*.cpp`/`*.h`
→ 0 hits. The `f_op_actor.cpp` diff confirms the `#include "dusk/coop/coop_context.h"` +
`#include "dusk/coop/coop_enemy.h"` + `<optional>` block at the top was removed; `d_cc_s.cpp`'s
`#include "dusk/coop/coop_combat.h"` is gone; `d_a_alldie.cpp`'s `#include "dusk/coop/coop_enemy.h"` is
gone. **The headless agent's claim is 100% for code.**

### B. Vanilla-equivalence contract (every `#if TARGET_PC` attachment point)
- **`d_cc_uty.cpp` + `d_attention.cpp`: ZERO diff** vs `723f7f42ea` (`git diff 723f7f42ea..HEAD -- src/d/d_cc_uty.cpp src/d/actor/d_attention.cpp | wc -l` → `0`). ✓ — `gF8` honored: `d_cc_uty`'s `TARGET_PC` blocks are the `invincibleEnemies` settings cheat + achievement signals (fork features, not coop combat); `d_attention.cpp:655` lock-on guard is M1 puppet code. Both untouched.
- **`f_op_actor.cpp`:** the enemy pre-guard (`puppetExecute`), the `ScopedEnemyTarget`/`hostNeedsContext` context push, and `hostOnExecuted` are all deleted; the `#if TARGET_PC / #else / #endif` around `fpcMtd_Execute` collapsed to the single vanilla direct call `ret = fpcMtd_Execute((process_method_class DUSK_CONST*)actor->sub_method, actor)` — the `#else` (non-PC) branch was the original direct call, now the only path. **Behavior-neutral without a session AND without TARGET_PC coop context — it's the vanilla path, period.** ✓
- **`d_cc_s.cpp`:** the `noteAtTgHit` combat intercept (the ONLY combat intercept, `:540`) + the `coop_combat.h` include are gone; `SetAtTgGObjInf` runs the vanilla `ChkShield` path. ✓
- **`d_a_alldie.cpp`:** the `hostRoomCleared`/`clientRoomClearGated` blocks in `actionCheck`/`actionTimer` + the `coop_enemy.h` include are gone; ALLDIE is vanilla (local, per-player). ✓
- **`d_a_e_yc.cpp` (6 sites) + `d_a_e_ai.cpp` (3 sites):** every `#if TARGET_PC ... daPy_getPlayerActorClass()/fopAcM_getContextPlayer() ... #else ... #endif` collapsed to the vanilla `dComIfGp_getPlayer(0)` / `static_cast<daPy_py_c*>(dComIfGp_getPlayer(0))`. ✓
- **`f_op_actor_mng.h`:** the `fopAcM_getContextPlayer` shim + the `dusk::coop::currentTargetPlayer()` forward decl are removed; the 7 `fopAcM_searchPlayer*` wrappers + `fopAcM_toPlayerShapeAngleY` + `fopAcM_seStartCurrent` now call `dComIfGp_getPlayer(0)` directly (vanilla). ✓
- **`d_a_player.h`:** the `daPy_getPlayerActorClass`/`daPy_getLinkPlayerActorClass` D3 shims + the `currentTargetPlayer()` forward decl are removed; both return the vanilla `dComIfGp_getPlayer(0)`/`dComIfGp_getLinkPlayer()`. ✓

The reverted files' `#else` (restored) path IS the original direct call, not a degraded shim. ✓

### C. Protocol v7 correctness
- **Type count 16→13:** `MsgType` enum (`protocol.h:120-136`) is `JoinRequest=1`..`WeatherChange=13`, 13 types. The 3 gone are `CombatIntent`/`CombatResult`/`RoomOwnership` (v6 ids 11/12/16 — confirmed by `protocol.cpp:262` comment + the selftest's "old CombatIntent frame at wire id 11" + "removed RoomOwnership wire id (16)"). ✓
- **`PayloadUnion`/`WireSize`/`ChannelFor`/determinism over 13:** `PayloadUnion` (`protocol.h:308-322`) has 13 members (no `combatIntent`/`combatResult`/`roomOwnership`); `WireSize` (`protocol.cpp:131-196`) switches all 13; `ChannelFor` (`protocol.h:333-343`) maps `PlayerState`/`GhostSnapshot`/`TimeSync`→unreliable, rest→reliable; `SerializeMessage`/`DeserializeMessage` (`protocol.cpp:198-294`) handle all 13. The selftest `RunDeterminismChecks` sweeps `JoinRequest..WeatherChange` (13) and asserts byte-identical re-serialization. ✓
- **`GhostSnapshotMsg` 34 B + `senderId`:** struct (`protocol.h:379-390`) is `u8 senderId + u8 flags + u16 entityId + u16 type + s16 angle + u16 animFrame + Vec3f pos + Vec3f speed` = `1+1+2+2+2+2+12+12 = 34 B`; `WireSize(GhostSnapshot)` (`protocol.cpp:181-184`) returns `1+1+2+2+2+2+12+12 = 34`; `ChannelFor(GhostSnapshot)=kChannelUnreliable`. The selftest asserts exactly this (`RunProtocolChecks`: `"GhostSnapshot wire size 34 B + unreliable channel (v7)"`). `senderId` is the first field ✓ so M5.2's `(senderId, entityId)` registry keying works; the `senderId == selfId()` self-echo drop is documented for the M5.2 host (`protocol.h:383-390` comment). ✓
- **`EnemyEventMsg` 4 B:** struct (`protocol.h:395-400`) is `u8 senderId + u8 eventId + u16 entityId = 4 B`; `WireSize(EnemyEvent)=4` (`protocol.cpp:188-191`); `ChannelFor(EnemyEvent)=kChannelReliable`; `EnemyEventId` has only `Died` (`protocol.h:155-158`, `Spawned`/`RoomClear`/`BossPhase` dropped). The selftest asserts `"EnemyEvent wire size 4 B + reliable channel (v7 trim)"`. ✓
- **Dead wire id size-rejection:** `RunProtocolChecks` (`selftest_main.cpp:386-399`) sends a type-16 frame → rejected (out of range, `WeatherChange=13` is the max), and a type-11/size-42 frame → rejected (`WireSize(TimeSync)=8 ≠ 42`). `RunV7NetOffRegression` (`:1982-1987`) asserts `WireSize(MsgType(16))==0` and `WireSize(MsgType(11))==8` ("wire id 11 is TimeSync now (8 B), not CombatIntent"). ✓
- **Version-history comment rewritten:** `protocol.h:74-88` — the v7 block describes the BREAKING DELETION (`CombatIntentMsg`/`CombatResultMsg`/`RoomOwnershipMsg` deleted, `EnemySnapshot`→`GhostSnapshot`, type count 16→13, "the M4.5 capstone 'M5 decides wire-vs-drop' notes resolve as DROPPED"). The stale "M5 bumps to v7 (it adds wire fields: spawn params, horse channel)" promise is GONE (confirmed: `grep 'spawn params' include/dusk/net/protocol.h` → 0). ✓
- **§4.2 field set with NO wire break pending (the agent's "deviation #1"):** the stub ships the exact §4.2 field set (34 B, all 8 fields, `senderId` present). `05-ghosts.md §4.2` says "34 B payload"; `protocol.h:382` comment says "34 B payload"; `WireSize` returns 34. **34 B IS the design's §4.2 size.** No wire break pending — the struct is wire-ready for M5.2. ✓
- **M5.1 sends ZERO ghost traffic despite the struct:** `coop.cpp:312-313` `OnGameMessage` has NO branch for `GhostSnapshot`/`EnemyEvent` ("wire-only until M5.2 lands — no receive branch exists, so an inbound ghost message is ignored"); `coop_enemy.cpp::onGameFrame` does only room-change reset, no sends; `coop.cpp::onGameFrame` calls `dusk::coop::enemy::onGameFrame()` (the stub) but never sends a ghost. The net-off row confirms off-session sends are refused. ✓

### D. Selftest integrity (run twice, 242 checks)
- **4 dropped suites gone:** `grep -nE 'RunM2RelayPolicyCheck|RunM4OwnershipTableCheck|RunM4RoomRoutingCheck|RunM4EntityStabilityCheck' src/dusk/net/selftest_main.cpp` → 0 hits. ✓
- **Surviving suites (18, in `main()` `:2009-2026`):** `RunProtocolChecks`, `RunDeterminismChecks`, `RunValidationChecks`, `RunRingPolicyChecks`, `RunRingOverflowChecks`, `RunClockChecks`, `RunGenerationGuardCheck`, `RunDuplicateJoinCheck`, `RunJoinAcceptValidationCheck`, `RunOversizedMetricCheck`, `RunHandshakeDemo`, `RunGameMessageDemo`, `RunV7NetOffRegression`, `RunM3TimeWeatherCheck`, `RunM35TimeWeatherFixCheck`, `RunM4WorldStageCheck`, `RunM46SessionRestartCheck`, `RunM4DiscoveryCheck`. All present, all called. ✓
- **Check count: 242, all `ok`, zero `FAIL`.** `./dusk_net_selftest 2>&1 | grep -cE '^  (ok|FAIL):'` → `242`; `grep -cE '^  ok:'` → `242`; `grep -E '^  FAIL:'` → empty. Run twice — both `PASS: all checks succeeded`, exit 0. ✓ (matches the expected "total 242".)
- **Net-off row (`RunV7NetOffRegression`, `:1964-1988`) asserts 6 REAL checks, not just prose:** `kProtocolVersion==7`; `JoinRequest==1 && WeatherChange==13` ("message ids compact 1..13"); `!idle.SendGameMessage(GhostSnapshot)` ("off-session: GhostSnapshot send refused (zero ghost sends)"); `!idle.SendGameMessage(EnemyEvent)`; `WireSize(MsgType(16))==0` ("no wire size exists for the removed RoomOwnership id"); `WireSize(MsgType(11))==8` ("wire id 11 is TimeSync now (8 B), not CombatIntent"). The game-side half of d-m8 (zero registry entries / `myRoomSearchEnemy` untouched) is honestly documented as "enforced by construction" (`:1976-1981` — the stub early-returns when off, the `fopAc_Execute`/`d_cc_s`/`d_a_alldie` hooks are deleted) rather than asserted at the session layer, which is correct: the selftest links game-free and cannot drive `fopAcM_myRoomSearchEnemy`. ✓

### E. Build link-green + no surviving subsystem depending on something deleted
- **Full game `dusklight` build: GREEN.** `cmake --build build/macos-default-relwithdebinfo --target dusklight` (forced: touched `coop.cpp`/`coop_enemy.cpp`/`f_op_actor.cpp`/`d_cc_s.cpp`/`d_a_alldie.cpp`/`d_a_e_yc.cpp`/`d_a_e_ai.cpp` + the reverted headers) → exit 0, `Linking CXX executable Dusklight.app/Contents/MacOS/Dusklight` succeeded, `INFO 33903 exports from 2053 objects`. The only warning is a pre-existing `-Wmultichar` in `libs/JSystem/.../JKRExpHeap.h:30` (`'HM'` magic) — unrelated to the shred. No unresolved externs from the deleted modules. ✓
- **Selftest `dusk_net_selftest` build: GREEN** (5 net TUs: `protocol`/`transport`/`discovery`/`selftest_main`/`session` + enet/fmt/Threads). ✓
- **No surviving subsystem silently depends on a deleted function:** the grep in §A found zero code refs to any deleted symbol; the full-game link would have failed on any unresolved extern (a header-only inline referencing a deleted `.cpp` function would link-fail too — it didn't). `coop.cpp` includes `coop_enemy.h`/`coop_time.h` (both survive); the surviving `coop.h` exports (`sessionActive`/`hostRole`/`selfId`/`remoteCount`/`isPuppet`/`puppetPlayerId`/`onLinkCreated`/`onLinkDestroyed`/`puppetExecute`/`sendPlayerState`/`puppetDrawHidden`/`onGameFrame`/`shutdown`/`sendGameMessage`/`rosterPresent`/`localRoomNo`/`localStageName`/`setWorldTime`/`setWorldWeather`/`worldTime`/`worldWeather`) all resolve to `coop.cpp` definitions. No `#include` of a deleted header survives (§A). ✓

### F. Docs / leftover surface
The code is clean. The docs are NOT all clean — see §1 MAJOR F1 + MINOR 3-4. Summary of what
contradicts HEAD:
- **`00-network.md §5` (`:148-158`) + §7 (`:280-296`) + §6 (`:320`):** list `CombatIntent`/`CombatResult`/`RoomOwnership`/`EnemySnapshot` as the live v6 message set + draw the dead combat-routing diagram; no supersede banner. **Would mislead the M5.2 dev who cross-references it** (MAJOR F1).
- **`implementation-plan.md` (`:173-262`):** M4 "IMPLEMENTED", M5 "Future" (old waves/horse/PvP plan), room-ownership section; no M5.1 shred record, no supersede banner (MINOR 3).
- **`TESTING.md` (`:5-12`, `:60`, `:66`, `:131-133`):** "M0–M4 landed" status, M4 `RoomOwnership` test reference, M4-ownership test rows describing the dead model; the selftest CODE is correct, only the prose is stale (MINOR 4).
- **`03-enemies.md` / `m2-design-notes.md`:** explicitly superseded by `05-ghosts.md:7-8`; no back-pointer banner, but they're historical investigation docs (the M5.2 dev's entry point is `05-ghosts.md`, not these). Low risk; a banner would be tidy but is not required to avoid misleading M5.2.
- **`05-ghosts.md` itself:** committed, correct, self-consistent (v7, 13 types, 34-B `GhostSnapshot` with `senderId`, 4-B `EnemyEvent`, §4.2 field set, M5.1 acceptance). No internal contradiction with HEAD. ✓

---

## 3. MUST-FIX before M5.2 + NICE-TO-HAVE

### MUST-FIX before M5.2 (code)
**None.** The shred is complete and regression-clean for code; HEAD builds green (full game + selftest);
the selftest is green (242, twice); the protocol v7 wire matches `05-ghosts.md §4.2` byte-for-byte; `senderId`
is present for M5.2's registry keying; zero ghost traffic ships; the dead wire ids are rejected. The M5.2
ghost sender lands on a clean, correct seam (`coop_enemy::onGameFrame` room-change reset +
`registerDynamicEnemy` dormant, `sendGameMessage` star-relay, `OnGameMessage` ready for a `GhostSnapshot`
branch).

### NICE-TO-HAVE (doc hygiene — none block M5.2, all are ≤1-line edits)
1. **`00-network.md §5` + §7 supersede banner** (MAJOR F1) — one line under each heading pointing to
   `05-ghosts.md §3` for the normative v7 13-type set + noting §7's combat routing is DROPPED. This is
   the one doc fix that materially de-risks M5.2 cross-reference confusion.
2. **`implementation-plan.md` M5 status banner** (MINOR 3) — mark M2/M3/M4 "DROPPED in M5.1
   (`05-ghosts.md`)" and record the M5.1 shred + the M5.2–M5.5 series from `05-ghosts.md §5`.
3. **`TESTING.md` status block + M4 ownership rows** (MINOR 4) — add the M5.1 line ("M5.1 SHRED landed:
   authority stack deleted, protocol v7, 242-check selftest green") and mark the M4 ownership rows
   DROPPED.
4. **"horse entity channel is M5" → M6** (`protocol.h:163`, `00-network.md:228`,
   `implementation-plan.md:46`) — `s/M5/M6/`; horses are M6 per `05-ghosts.md §8`, and M5 is the ghost
   pivot.
5. **`coop_enemy.cpp:25` comment** (MINOR 1) — optionally drop the `SetTgHitSynthetic` name ("the old M2
   synth-collider pointer discipline"); it's accurate history, not a dangling ref, so leaving it is fine.

---

*Review artifacts: every file/line reference verified on branch `net-coop` @ HEAD
(`1aa436a4d8`). Forced rebuild of `dusk_net_selftest` (5 TUs) + `dusklight` (1188 TUs) both green;
selftest run twice (242 checks, 0 FAIL, exit 0). No source files modified.*
