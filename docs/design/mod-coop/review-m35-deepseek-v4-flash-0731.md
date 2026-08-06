# M3.5 adversarial review — time & weather sync fix pass (deepseek-v4-flash-0731)

Reviewed range: `976ec63789..HEAD` (commits `3269fbf312`, `fd10af62fc`,
`e71cdfb96d`, `b7af592e43`, `0557c5f7b8`), branch `net-coop`. Scope: the M3.5
fixes against the two M3 reviews (deepseek MAJOR 1 + MINORs 1-8; glm MAJOR 1 +
MINORs 1-3), the amended `00-network.md §5/§6/§8`, and the committed
`modelCallBack` puppet suppression. The debug-agent's pre-`976ec63789` puppet
commits are treated as verified-good (user) and reviewed only for M3.5
interaction. Independent evidence: full clean rebuild, forced-clean selftest
run, net-off boot, live host+client session, and line-by-line comparison
against vanilla `d_kankyo.cpp` / `d_a_kytag06.cpp`.

## Evidence gathered (all real)

- **Full forced rebuild**: `ninja -t clean dusklight` → 2053 objects
  recompiled, 33903 exports, `dusklight-stub` linked, app bundle assembled.
  The only warning in the whole log is the pre-existing `ld: warning:
  ignoring duplicate libraries`; **zero compile warnings in any M3.5-touched
  TU** (`coop_time.cpp`, `d_a_alink.cpp`, `selftest_main.cpp`,
  `coop_time_logic.h`). (The one failure in the run was the known build-system
  quirk: `ninja -t clean` deletes the `.app` bundle dir, after which the
  `COPY_OSX_CONTENT_FILE` rule's `cmake -E copy` fails on the missing parent
  dir; recreating `Dusklight.app/Contents/{MacOS,Resources,Frameworks}` and
  re-running completes the bundle — pre-existing, not a code issue.)
- **Selftest forced rebuild** (`ninja -t clean dusk_net_selftest` → rebuild →
  run): **218/218 ok, `PASS: all checks succeeded`** — all M1/M2/M3 checks
  plus the 11 new M3.5 checks (`RunM35TimeWeatherFixCheck`).
- **Net-off boot** (~75 s): game reaches gameplay (`fpcNm_KANKYO_e` process
  created, `coop: real Link registered (pid 19)`), **zero** TimeSync /
  WeatherChange / puppet / session activity, no errors — M3/M3.5 hooks inert.
- **Live host+client** (~15 min, 127.0.0.1, v5): host session started, client
  joined as player 1, puppets spawned active on both sides, 60 Hz PlayerState
  streaming both ways, `WorldInit stage '' room 0` (join), zero errors/asserts
  on either side. Wire-level cadence check via per-process UDP counters: the
  client received **2762 more packets than it sent over 30 s (≈1.6 pps net
  inbound excess)** — consistent with a 1 Hz TimeSync + rare events; a 60 Hz
  TimeSync would show ≈ +59 pps (in-pps ≈ 92 with avg 685 B vs the ~60 Hz
  snapshot baseline).
- Working tree stayed clean of source changes (only my review file added; one
  unrelated pre-existing working-tree edit to `implementation-plan.md` — M4
  room-ownership planning, not mine, not touched).

---

## VERDICT

**M3.5 resolves MAJOR 1 and the M3 MINORs it was tasked with, correctly and
provably.** HEAD is buildable (full clean rebuild), the selftest is green on a
forced clean rebuild (218/218, incl. a non-tautological cadence regression
guard), and both net-off and live host+client runs are regression-clean.
**No BLOCKERs, no MAJORs.** Four MINORs (all non-blocking, all fixable in a
few lines): the puppet suppression is human-form-only (wolf puppets still run
per-joint corrections), the re-seed race fix is the one M3.5 change with no
automated test, the `d_kankyo.cpp` hook-site comment is still stale while the
corrected comment lives inside `onStageCreate`, and two docs lag the code
(04 §5.11 "uniform 2x", TESTING.md status). M3 MINOR 6 (dice UNK6 drain) is
unchanged and accepted-cosmetic.

| M3 finding | M3.5 fix | Verdict |
|---|---|---|
| deepseek MAJOR 1 (TimeSync 60 Hz) | `NetClock::AtRate(1)` + `TimeSyncDue` + cadence selftest | ✅ resolved, provably |
| deepseek MINOR 1 (thunder) | `NextThunderMode` defer-to-wire | ✅ resolved |
| deepseek MINOR 2 / glm MINOR 3 (darkworld tag) | `using_time_control_tag == 0` gate | ✅ resolved, vanilla-exact |
| deepseek MINOR 3 (pond fallback) | `RatePerTick` 1× fallback | ✅ resolved, vanilla-exact |
| glm MINOR 1 (re-seed race) | `SeedTargets` `!g_time.valid` gate | ✅ resolved by reasoning; ⚠ no test (MINOR) |
| deepseek MINOR 4 (onStageCreate comment) | comment corrected (2 of 3 sites) | ✅ in `coop_time.cpp`/`coop_time.h`; ⚠ stale at `d_kankyo.cpp:8375` (MINOR) |
| deepseek MINOR 5 (darkworld bit) | annotated | ✅ documented as informational |
| deepseek MINOR 7 (selftest coverage) | `coop_time_logic.h` + 11 checks | ✅ largely addressed |
| deepseek MINOR 8 / glm MINOR 2 (docs §5/§8) | `00-network.md` v5 rows | ✅ §5/§6/§8 match `protocol.h` v5 |

---

## RANKED FINDINGS

### MINOR A — Puppet `modelCallBack` suppression is human-form-only; wolf puppets still run per-joint corrections

**Evidence.** The committed gate sits only in `daAlink_c::modelCallBack`
(`src/d/actor/d_a_alink.cpp:2428-2439`, inside `#if TARGET_PC`). The wolf
skeleton's callback chain — `daAlink_wolfModelCallBack` → `wolfModelCallBack`
(`d_a_alink.cpp:2534-2541`) — is registered on the wolf model's joints
(`d_a_alink_swindow.inc:224`) and runs **unconditionally** for wolf puppets:
`jointControll(i_jointNo)` + `setWolfFootMatrix()` + `changeWolfBlendRate()`.
`setWolfFootMatrix` modifies the anm-matrix buffer (reads `getAnmMtx` into
`mFootData*`, then `setMatrixWorldAxisRot`-style writes back —
`d_a_alink_wolf.inc:1883+`), exactly the class of post-paste overwrite the
suppression exists to prevent. The 8cd182d4d3 envelope recompute runs in
`ApplyPuppetState` (execute), so a wolf puppet's draw-time callback edits to
`mpAnmMtx` leave the recomputed envelope matrices stale vs the rigid joints —
the same "some vertices not following" artifact, in wolf form.

**Assessment.** Pre-existing (the uncommitted suppression was already
human-only), so not an M3.5 regression; the user's verified-good sessions
presumably covered human form. Wolf co-op is a narrower path (form-swap +
wolf skeleton + `setWolfFootMatrix` under `!getSumouMode()`), but the fix is
one line and symmetric.

**Fix.** Mirror the gate in `wolfModelCallBack`:
`if (dusk::coop::isPuppet(this)) return true;` inside `#if TARGET_PC`, then
visually verify a wolf-form puppet (feet/tail/weighted back) against the
sender. Alternatively, gate both callbacks via one early-return helper.

### MINOR B — The re-seed race fix (glm MINOR 1) is the only M3.5 fix with no automated test

**Evidence.** `SeedTargets`' `if (!g_time.valid)` gate
(`src/dusk/coop/coop_time.cpp:456-477`) lives in the game TU, which the
selftest cannot link (per the module header's own rationale,
`coop_time_logic.h:1-18`). `RunM35TimeWeatherFixCheck` table-tests cadence,
`TimeSyncDue`, `DeriveWeatherFrom`, `NextThunderMode`, and `RatePerTick`, but
nothing exercises the seed-adoption decision. This is the riskiest M3.5 change
at 1 Hz (a roster-refresh `WorldInit` racing ahead of — or behind — an
unreliable `TimeSync` is a packet-ordering property, not a pure table), so its
correctness currently rests on review alone.

**Assessment.** The fix itself is correct (verified below in VERIFIED-OK, by
channel-architecture argument: `WorldInit`/`WeatherChange` are both reliable
on channel 0 so they are ordered with each other; `TimeSync` is unreliable on
channel 1 so it can overtake them — the `!g_time.valid` gate is the right
shape). The gap is testability, not correctness.

**Fix.** Extract the decision as a pure predicate in `coop_time_logic.h`
(e.g. `bool SeedTimeAdopted(bool anyTimeSyncReceived)` or a small
`SeedDecision` table over `(timeValid, worldTime != seen, timeSyncReceived)`)
and add a table test: first-join adopts, later roster-refresh does not, and a
world-state change with `timeValid` still refreshes weather.

### MINOR C — Stale "before room-layer resolution" comment remains at the hook site

**Evidence.** `src/d/d_kankyo.cpp:8374-8376` (the `dKy_Create` hook that calls
`onStageCreate`) still says: *"a synced client re-asserts the replicated time
into the save before room-layer resolution (04 §5.10)"*. The M3.5 correction
(deepseek MINOR 4) fixed the comment inside `onStageCreate`
(`coop_time.cpp:563-571`) and in `coop_time.h:82-90` — both now correctly say
the start-room LAYER is resolved BEFORE this hook (`dStage_Create`:
`dStage_roomInit` `d_stage.cpp:2758` → `dComIfG_play_c::getLayerNo(0)` which
reads save-time-derived state, then `dKankyo_create` `:2763` → `dKy_Create`).
The two statements now contradict each other.

**Fix.** Update the `d_kankyo.cpp:8374-8376` comment to match the corrected
one (one-line wording change; behavior is already the accepted 04 §5.10 worst
case).

### MINOR D — Docs lag the code: 04 §5.11 "uniform 2x" and TESTING.md status

- `docs/design/mod-coop/04-time-weather.md:317` (edge case 5.11) still
  blesses *"otherwise `rate=3` (uniform 2x)"*; M3.5 now replicates the exact
  vanilla segment rule with a 1× fallback (`RatePerTick`), so the doc should
  say the client replicates the exact rule (the 2× approximation is gone).
- `docs/design/mod-coop/TESTING.md:6` status line still says *"M3 …
  mid-implementation … `dusk_net_selftest` is temporarily red"* — stale; M3.5
  is committed and the selftest is green.
- Not fixed by M3.5 (unchanged from M3, accepted-cosmetic, noted for the
  record): M3 MINOR 6 — the dice `UNK6` drain rate mismatch (host
  `raincnt -= 2`/frame, `d_a_kytag06.cpp:232-239`, vs client `DiceRainMinus`
  ≈0.75/frame), and the UNK6-with-rain derive collapsing to RainLight/RainHeavy
  (hold/climb) instead of drain. Bounded by the transient UNK6 window.

---

## VERIFIED-OK (per-fix evidence)

1. **MAJOR 1 — TimeSync at real 1 Hz.** `NetClock::AtRate(1)`
   (`include/dusk/net/clock.h:36-44`) sets `rateHz_ = 1` and
   `intervalUs_ = 1000000 + (1000000 % 1 != 0 ? 1 : 0) = 1,000,000`; `Tick()`
   returns true exactly once per interval with single-step catch-up and no
   bursts. Game wiring: `g_syncClock = net::NetClock::AtRate(1)`
   (`coop_time.cpp:70-74`), `cadenceDue = g_syncClock.Tick(NowUs())` per frame,
   send gate `TimeSyncDue(SyncDueInput{cadenceDue, stageChanged, rate,
   g_lastRate})` (`coop_time.cpp:287-296`; `coop_time_logic.h:169-182`) —
   cadence tick **OR** stage change **OR** rate transition. The selftest
   drives the production objects over simulated 60 Hz frames and asserts: ≤2
   ticks in 250 ms (0 actual); the default 60 Hz clock fires ~15× in the same
   window (**the regression guard — the check demonstrably fails on the old
   implementation**); exactly one tick per second and none between; single
   tick after a 4 s stall; 2 cadence + 1 stage + 1 rate = 4 sends over a 2 s
   sim; the immediate stage/rate sends still fire inside a quiet 250 ms
   window. Live corroboration: the client's per-socket packet counters show a
   ≈ +1.6 pps inbound excess over 30 s (1 Hz TimeSync + rare events), not the
   +59 a 60 Hz TimeSync would produce. The rate-advance machinery
   (`AdvanceStep`, frozen-hold, pond segments) is no longer shadowed by
   per-frame absolute adoption — the 1 Hz receipt is now the only re-adopt.
2. **Re-seed race (glm MINOR 1).** `SeedTargets` (`coop_time.cpp:453-477`)
   adopts TIME only when `!g_time.valid` (first join: `JoinAccept`/`WorldInit`
   are reliable and nothing can have raced ahead), and always adopts weather.
   Correctness at 1 Hz: `WorldInit` (roster-refresh re-broadcast) and
   `WeatherChange` are both reliable on `kChannelReliable` (0)
   (`protocol.h:626-628`) — ordered with each other — while `TimeSync` is
   unreliable on channel 1 and can overtake an in-flight `WorldInit`; the gate
   prevents that older reliable copy from regressing the clock, and weather
   re-seed is safe because no fresher per-frame weather stream exists. First-
   join seeding intact: `g_time.valid` starts false, `onGameFrame` resets it
   on session teardown (`coop_time.cpp:662-668`), and the mid-game joiner
   still gets the host's clock via `dComIfGs_setTime/Date` inside the gate.
   Edge: a client whose early TimeSyncs all drop holds the `WorldInit` time
   until the next 1 Hz receipt — acceptable (self-corrects).
3. **Thunder policy (deepseek MINOR 1).** `NextThunderMode`
   (`coop_time_logic.h:104-131`): Thunder modes force 1; **Cloudy clears only
   when `wireThunder == 0`** (a held synced thunder on a non-dice stage —
   wether-proc case 5 with rain drained — is re-asserted, no longer undone by
   `ThunderPerMode`); Clear clears a 1 only when the wire says 0 (kytag00's
   mMode == 2 survives); Rain/Snow never touch it. Dice-stage behavior
   unchanged: the host's dice-Cloudy move clears `mMode` and publishes
   `thunder=0`, so the client still clears — verified by the selftest table
   (11 cases incl. `{Cloudy,1,x→1}` and `{Cloudy,0,2→0}`). The
   Clear+thunder=1-with-current-0 case (which would hold 0 while the host
   flashes) is unreachable in the engine (thunder only coexists with
   colpat ≥ 1, which derives Cloudy — the selftest's no-rain cases document
   this). `DeriveWeather` now always carries the live thunder bit on the wire
   (asserted).
4. **Darkworld gate (deepseek MINOR 2 / glm MINOR 3).** The replica's
   `dark_daytime` advance is inside `if (env.using_time_control_tag == 0)`
   (`coop_time.cpp:506-517`), matching vanilla `setDaytime` exactly —
   `d_kankyo.cpp:1601-1606` wraps the entire darkworld branch (`dark_daytime
   += time_change_rate`, week wrap, `daytime = 0`) in the same gate. The
   `env.daytime = 0.0f` pin stays outside the gate — that is the pre-existing
   M3 design (accepted in both M3 reviews; twilight lighting is driven by
   `dark_daytime`, and the divergence only exists under an active kytag11 tag
   in a twilight stage, which the two reviewers rated cosmetic). The tail
   (`daytime >= 360` clamp, save/audio-clock writes, `using_time_control_tag
   = 0` reset) is vanilla-identical (`coop_time.cpp:528-537` vs
   `d_kankyo.cpp:1666-1675`). Non-PC path untouched (additive `#if TARGET_PC`,
   no `#else`; M3.5's diff to `d_kankyo.cpp`/`d_a_kytag06.cpp` is empty).
5. **Pond fallback (deepseek MINOR 3).** `RatePerTick`
   (`coop_time_logic.h:139-161`): `kTimeRatePond2x` + pond stage → 0.036 in
   300-60 (incl. boundaries, vanilla `>= 300.0f || <= 60.0f`), 0.024 in
   150-195, **0.012 elsewhere**; non-pond stage → 0.012. Matches vanilla
   `d_kankyo.cpp:1575-1584` (base `+= time_change_rate` = 1×, +2 extra in
   300-60 = 3×, +1 extra in 150-195 = 2×, 1× everywhere else) as a single
   per-tick advance. Selftest table covers both windows, boundaries (300, 45,
   150, 180), the 1× fallback (100, 240), and the non-pond-stage case.
   `AdvanceStep` delegates with the same pond-stage check as before
   (`coop_time.cpp:334-342`).
6. **Docs (deepseek MINOR 8 / glm MINOR 2).** `00-network.md` §5 rows now
   match `protocol.h` v5: `TimeSync` = absolute phase f32 0..360 + day + rate
   + flags at a real 1 Hz (`NetClock::AtRate(1)` gate; immediate on stage/rate
   change); `TimeEvent` = event id + time + day; `WeatherChange` = mode +
   thunder + intensity + colpat; `WorldInit` rows carry time + weather (v5).
   §6 cadence note and §8 payload text updated. `kProtocolVersion = 5` with
   the v5 changelog (`protocol.h:55-61`) unchanged and consistent.
7. **modelCallBack suppression (committed, 0557c5f7b8).**
   (a) Non-PC path byte-identical: the block is additive inside `#if
   TARGET_PC`, `coop.h` is included under the same guard (`d_a_alink.cpp:54-
   58`) — with `TARGET_PC=0` the function is textually vanilla.
   (b) Local Link and everything else unaffected: `dusk::coop::isPuppet(this)`
   (`coop.cpp:884-888`, `EntryForPid(fopAcM_GetID(link))`) is false for the
   real Link, so `jointControll`/`setUpperFront`/`setFootMatrix`/
   `setArmMatrix`/`changeBlendRate` run as vanilla; the `param_1 == 1`
   `resetRootMtx` path in the static wrapper is separate and not gated; the
   hat/face callbacks (`headModelCallBack`, `d_a_alink.cpp:2477+`) are not
   suppressed and correctly re-derive the hat from the pasted head matrix.
   The "sender's corrections are baked into the synced matrices" claim holds:
   `sendPlayerState` runs at the end of the real Link's execute
   (`d_a_alink.cpp:19074-19080`), after `setMatrix → modelCalc` (the J3D joint
   callbacks fire during calc, so the sender's `setFootMatrix` etc. already
   modified `mpAnmMtx`), and reads `getAnmMtx(j)`.
   (c) Interaction with 8cd182d4d3: the suppression protects the envelope
   recompute — without it, `setFootMatrix`'s `setMatrixWorldAxisRot` writes to
   `mpAnmMtx` at draw time would stale the envelopes computed in
   `ApplyPuppetState`. The committed state is the state that was already in
   the working tree during the user's verification (per the commit message:
   "The uncommitted #if TARGET_PC early-return … stays, with the comment
   corrected"), so the verified-good rendering is unchanged — the only delta
   is the comment. (Gap: wolf form — finding MINOR A.)
8. **Regression.**
   - Full clean rebuild of `dusklight`: all M3.5 TUs compile+link, zero new
     warnings; `d_kankyo.cpp`/`d_a_kytag06.cpp` have **zero diff** in M3.5
     (M1/M2/M3 hooks byte-identical to 976ec63789); the M1/M2 path is
     untouched by construction (the M3.5 diff touches 7 files: docs,
     `coop_time.{h,cpp}`, `coop_time_logic.h` (new), `clock.h`,
     `d_a_alink.cpp` (draw-callback only), `selftest_main.cpp`).
   - `dusk_net_selftest`: 218/218 PASS on a forced clean rebuild — all M3
     wire/routing checks (round-trips, sizes, channels, JoinAccept/WorldInit
     carry, host→all, rogue-consumption-without-relay) plus the 11 M3.5
     checks.
   - Net-off boot: gameplay reached, KANKYO process normal, real Link
     registered, zero time/weather coop activity, zero errors.
   - Live host+client: join (v5), roster, puppets spawn active, 60 Hz
     streaming both ways, zero errors/asserts over ~15 min.
   - No M4 creep: no join-warp/unlock-gate/forms/host-migration code in the
     range; `shutdown()` resets all statics.
   - Commit hygiene: 5 scoped commits (`3269fbf312` cadence+selftest,
     `fd10af62fc` re-seed, `e71cdfb96d` thunder/darkworld/pond, `b7af592e43`
     docs, `0557c5f7b8` alink); each message describes its diff accurately.
   - onStageCreate comment + darkworld flag annotations present and accurate
     (see MINOR C for the one stale site).
9. **Selftest quality (deepseek MINOR 7).** The `coop_time_logic.h`
   extraction is a real fix for the "shipped untested" failure mode: the
   selftest now drives the **production** `NetClock::AtRate(1)`, `TimeSyncDue`,
   `DeriveWeatherFrom`, `NextThunderMode`, `RatePerTick` (not copies) — and
   the cadence check is non-tautological (it asserts the old 60 Hz clock
   fires ~15× in the window, so a regression to 60 Hz fails the build). The
   only uncovered decision is `SeedTargets` (MINOR B).

---

## MUST-FIX before M4

**None.** All findings are non-blocking:

1. **MINOR A** (wolf-form puppet callback) — add the same `isPuppet` gate to
   `wolfModelCallBack` and visually check a wolf puppet. Smallest real
   correctness gap in the committed suppression.
2. **MINOR B** — extract the seed-adoption decision into `coop_time_logic.h`
   and table-test it (the one M3.5 fix without coverage).
3. **MINOR C** — fix the stale `d_kankyo.cpp:8374-8376` comment (one line).
4. **MINOR D** — update `04-time-weather.md §5.11` (exact rule now, not
   "uniform 2x") and `TESTING.md` status line.
