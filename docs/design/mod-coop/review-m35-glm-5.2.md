# M3.5 adversarial review — time & weather sync fix pass (glm-5.2)

Review range: `976ec63789..HEAD` (commits `3269fbf312`, `fd10af62fc`,
`e71cdfb96d`, `b7af592e43`, `0557c5f7b8`), branch `net-coop`. Scope: the five
M3.5 fix commits against the two M3 reviews
(`review-m3-deepseek-v4-flash-0731.md` MAJOR 1 + MINORs 1-8;
`review-m3-glm-5.2.md` MAJOR 1 + MINORs 1-3). The puppet pose-envelope
commits before `976ec63789` (`8cd182d4d3`, `349fa0c64e`, `976ec63789`) are
out of scope except where M3.5's `modelCallBack` suppression touches the
same render path.

## Evidence gathered (all real)

- **Forced clean rebuild of the game**: `ninja -t clean dusklight` (2247
  files) → `ninja dusklight` → clean compile+link of every M3.5-touched TU
  (`coop_time.cpp`, `coop_time_logic.h` [new], `clock.h`, `d_a_alink.cpp`,
  `coop_time.h`), 33903 exports / 2053 objects. Only the pre-existing
  `JKRExpHeap.h:30` multi-character-constant warning and the harmless
  `ld: ignoring duplicate libraries` warning (both present on `149b4bc896`).
  One PCH-race failure on the first parallel pass (`touch_controls.cpp.o`
  raced the PCH generate) — re-ran `ninja dusklight` and it completed clean;
  this is a build-system parallelism artifact, not a code defect.
- **Forced clean rebuild of the selftest**: `ninja -t clean
  dusk_net_selftest` → `ninja dusk_net_selftest` → clean compile+link.
- **Selftest run**: `./dusk_net_selftest` → **`PASS: all checks succeeded`**,
  including every M3 check (round-trips, wire sizes, channel mapping,
  JoinAccept/WorldInit carry, host→all broadcast, rogue-client consumption
  without relay) **and** the new `RunM35TimeWeatherFixCheck` (11 M3.5
  assertions: 1 Hz cadence + regression guard + immediate sends + derive
  table + thunder policy + pond table).
- **Source cross-checks** against vanilla: `d_kankyo.cpp:1547-1606`
  (setDaytime advance gate + darkworld branch + pond block),
  `d_stage.cpp:2744-2773` (dStage_Create ordering), `d_a_kytag06.cpp:508-517`
  (wether-5 case 5), `protocol.h:404-435` (v5 wire structs),
  `coop.cpp:884-891` (isPuppet), `coop.cpp:543-560` (8cd182d4d3 envelope
  recompute).

---

## VERDICT

**M3.5 resolves MAJOR 1 and every MINOR flagged in the two M3 reviews. HEAD
is buildable, selftest-green, and regression-clean.**

| M3 finding | Fix | Status | Evidence |
|---|---|---|---|
| deepseek MAJOR 1 / glm MAJOR 1 — TimeSync 60 Hz, not 1 Hz | `NetClock::AtRate(1)` + `TimeSyncDue` + cadence selftest | **RESOLVED** | clock.h:38-46 (AtRate factory, intervalUs_=1000000); coop_time.cpp:74 (`g_syncClock = NetClock::AtRate(1)`); coop_time.cpp:93-97 (NowUs = steady_clock, real wall clock); selftest_main.cpp:1280-1316 (cadence + regression guard) |
| deepseek MINOR 1 / glm MINOR 1 — re-seed race regresses a fresher TimeSync | `SeedTargets` adopts TIME only when `!g_time.valid` | **RESOLVED** | coop_time.cpp:466-477; ordering verified: `g_session.Update()` drains inbox (TimeSync→onGameMessage sets `g_time.valid`) before `timeweather::onGameFrame()` runs SeedTargets (coop.cpp:1062-1074, session.cpp:180-193) |
| deepseek MINOR 1 — `ThunderPerMode` clears synced thunder | `NextThunderMode` defers to `g_weather.thunder` | **RESOLVED** | coop_time_logic.h:121-143; coop_time.cpp:423-429; selftest_main.cpp:1352-1390 |
| deepseek MINOR 2 / glm MINOR 3 — darkworld advance missing `using_time_control_tag` gate | wrapped in `if (env.using_time_control_tag == 0)` | **RESOLVED** | coop_time.cpp:508-513; mirrors vanilla d_kankyo.cpp:1547,1606 |
| deepseek MINOR 3 — pond fallback 2×, not vanilla 1× | fallback now `0.012f` | **RESOLVED** | coop_time_logic.h:172-176; selftest_main.cpp:1395-1422 |
| deepseek MINOR 4 — `onStageCreate` "before room-layer resolution" comment | corrected to "after start-room layer resolution" | **RESOLVED** | coop_time.h:85-90, coop_time.cpp:562-574; ordering verified d_stage.cpp:2758(roomInit)→2763(dKankyo_create)→d_kankyo.cpp:8381(onStageCreate) |
| deepseek MINOR 5 — darkworld flag bit unused | annotated as informational/diagnostics + M4+ | **RESOLVED** | protocol.h:195-199 (bit 0 annotation); coop_time.cpp:242-246 |
| deepseek MINOR 7 — selftest has no game-side coverage | `coop_time_logic.h` pure core + table tests in selftest | **RESOLVED** | coop_time_logic.h (game-free); selftest_main.cpp:1268-1422 (5 check sections) |
| deepseek MINOR 8 / glm MINOR 2 — 00-network.md §5/§8 stale | updated to v5 wire rows | **RESOLVED** | 00-network.md §5 (TimeSync/TimeEvent/WeatherChange rows), §4/§5 WorldInit, §6 cadence note, §8 payload text |
| glm MINOR 3 (darkworld tag) = deepseek MINOR 2 | same fix | **RESOLVED** | (see above) |
| modelCallBack suppression (committed as defense-in-depth) | `#if TARGET_PC` early-return for puppets | **VERIFIED-OK** | d_a_alink.cpp:2427-2442; non-PC path byte-identical; isPuppet false for local Link |

---

## RANKED FINDINGS

### BLOCKER
None.

### MAJOR
None.

### MINOR 1 — Host `WeatherChange` publish-on-mode-change-only: a thunder-only transition is not published (pre-existing, out of M3.5 scope)

**Evidence:** `coop_time.cpp:309-314` — the `WeatherChange` send gate is
`if (mode != g_lastMode || snowDrift || stageChanged)`. The `thunder` field
is carried on the wire but a transition that flips `mThunderEff.mMode`
without changing the derived `WeatherMode` (and without a stage change)
sends no `WeatherChange`, so clients never learn the new thunder bit. The
client's `g_weather.thunder` then holds the last `WeatherChange`'s value
indefinitely, and `ThunderPerMode` (which now correctly defers to that
value) keeps re-asserting the stale bit.

**Why this is not a regression:** M3 had the identical publish policy
(`mode != g_lastMode`); the M1 fix only changed the *client-side* policy
(`ThunderPerMode` no longer fights the wire). The residual is a *host-side*
publish gap that existed before M3.5 and is independent of the M1 fix. The
deepseek M1 scenario (wether-5 case 5: rain drains 0→mode collapses Cloudy
while thunder holds) *does* trigger a mode change (the prior RainLight/
ThunderLight → Cloudy), so it *is* published and the M1 fix correctly
preserves the carried thunder on receipt. The unmapped case is narrower:
a `mThunderEff.mMode` flip with the derived mode already at its post-flip
value (e.g. a kytag00 thunder-area tag arming on a stage whose colpat is
already ≥1 → mode stays Cloudy, thunder 0→1).

**Trigger:** narrow (kytag00 area-tag arming / wether-proc transition that
doesn't move colpat, on a non-dice stage). Self-correcting on the next
mode change or stage change. Cosmetic (lightning shown on host only).

**Fix (future, not M3.5):** add `thunder != g_lastThunder` (and track
`g_lastThunder`) to the `WeatherChange` send condition, or publish a
`WeatherChange` on any thunder-bit edge. Trivial; ~4 lines + a selftest
row. Not a must-fix for M4 — the M1 client-side fix is complete and correct
for its scoped finding.

---

## VERIFIED-OK

1. **MAJOR 1 — real 1 Hz cadence (not tautological).** `clock.h:38-46`
   `NetClock::AtRate(u32 hz)` is a named factory (the doc-comment explains
   why a `NetClock(u32 hz)` ctor overload was rejected: it would silently
   re-bind existing `NetClock(1000000)` call sites from "arm at 1 s" to
   "1 MHz ticks"). `AtRate(1)` sets `intervalUs_ = 1000000` (with the
   `1000000 % hz != 0` round-up). `coop_time.cpp:74` instantiates
   `g_syncClock = NetClock::AtRate(1)`. `NowUs()` (`coop_time.cpp:93-97`)
   is `std::chrono::steady_clock` — a real wall clock, so in the live game
   the gate fires once per real second, not once per 60 sim frames. The
   selftest (`selftest_main.cpp:1280-1316`) drives the *real* `AtRate(1)`
   clock over simulated 60 Hz frames and asserts: ≤2 ticks in a 250 ms
   window (actually 0), exactly 1 tick at the 1 s boundary, quiet just
   after, and a 4 s stall catching up in a *single* tick (no burst). It is
   **not tautological**: a parallel regression guard runs the *default*
   60 Hz `NetClock()` over the same window and asserts `> 10` ticks —
   proving the check would have caught MAJOR 1. The `Tick` catch-up math
   (`(nowUs - nextTickUs_) / intervalUs_ + 1`, single return) is correct
   for the 1 Hz interval.

2. **Immediate sends survive the 1 Hz gate.** `TimeSyncDue` (`coop_time_logic.h:188-190`)
   is `cadenceTick || stageChanged || rate != lastRate`. The selftest
   (`selftest_main.cpp:1320-1345`) simulates 2 s + 1 frame with a stage
   change at f=30 and a rate transition at f=45 and asserts `sends == 4`
   (2 cadence @ f=60,120 + 1 stage @ f=30 + 1 rate @ f=45); a second
   sub-check asserts `immediate == 2` (stage + rate) inside a 250 ms
   window with no cadence ticks. The `g_syncClock.Reset(NowUs())` on
   stage change (`coop_time.cpp:273`) re-arms the 1 Hz cadence so it
   doesn't double-fire; the immediate stage-change send covers the
   transition. Verified.

3. **Re-seed race — fix is correct at 1 Hz; first-join seeding works.**
   `SeedTargets` (`coop_time.cpp:466-477`) adopts TIME only when
   `!g_time.valid`. `g_time.valid` is cleared on session teardown
   (`coop_time.cpp:645`, `shutdown()` at `:692`) and set true by a
   `TimeSync` receipt (`onGameMessage`, `coop_time.cpp:606`). Ordering
   verified: `coop::onGameFrame()` calls `g_session.Update()` (which
   drains the inbox — `session.cpp:180-193` — and dispatches
   `TimeSync`→`timeweather::onGameMessage`, setting `g_time.valid`)
   *before* `timeweather::onGameFrame()` runs `SeedTargets`
   (`coop.cpp:1062-1074`). So if a `TimeSync` and a roster-refresh
   `WorldInit` land in the same frame, `g_time.valid` is already true
   when `SeedTargets` runs → the older `WorldInit` time is not adopted.
   First-join: `g_time.valid` is false → `SeedTargets` adopts the
   `JoinAccept`/`WorldInit` time and writes it to the save
   (`dComIfGs_setTime/Date`) so a mid-game joiner starts with the host's
   clock. The `g_weather` re-seed is unconditional (correct —
   `WeatherChange` is reliable like `WorldInit`, same channel, ordered,
   no fresher per-frame stream). The residual cost (a 3rd-player join
   within ~1 s of the 2nd holds the older `JoinAccept` time until the
   next `TimeSync`, ≤1 s) is bounded, self-correcting, and documented in
   the `onGameFrame` comment. Verified.

4. **Thunder policy — synced thunder no longer fought; dice behavior
   unchanged.** `ThunderPerMode` (`coop_time.cpp:423-429`) delegates to
   `NextThunderMode(g_weather.mode, g_weather.thunder, current)`
   (`coop_time_logic.h:121-143`): ThunderLight/Heavy → 1; Cloudy →
   `wireThunder == 0 ? 0 : 1` (the fix — a held synced thunder with a
   Cloudy derivation re-asserts 1 like the host's per-frame proc); Clear
   → clears a 1 only when `wireThunder == 0` (kytag00's `mMode == 2`
   survives); Rain/Snow → untouched. Selftest table
   (`selftest_main.cpp:1352-1390`) covers all 11 cases. Dice stages: the
   dice proc writes `mThunderEff.mMode` directly (0 on Cloudy, 1 on
   thunder modes), the derive carries that bit, and on a dice Cloudy
   `wireThunder == 0` → `NextThunderMode` clears (same as M3). The
   `DeriveWeather` dice-mode mapping (`coop_time_logic.h:62-82`,
   `kDiceModeToWeather[7]`) matches `d_kankyo.h:973-979`
   (`DICE_MODE_THUNDER_HEAVY_e == 5 == kDiceModeThunderHeavy`) and the
   dice proc (`d_a_kytag06.cpp:193-235`). Verified.

5. **Darkworld gate — mirrors vanilla exactly; non-PC path untouched.**
   `clientClockReplica` (`coop_time.cpp:508-513`) wraps
   `env.dark_daytime += env.time_change_rate` in
   `if (env.using_time_control_tag == 0)`, matching vanilla
   `d_kankyo.cpp:1547` (the `if (using_time_control_tag == 0)` gate wraps
   *both* the `!dKy_darkworld_check()` day branch and the `else` darkworld
   branch, `d_kankyo.cpp:1547-1606`). The `darktime_week++` + wrap + the
   `env.daytime = 0.0f` pin are preserved. The non-PC path is untouched:
   `clientClockReplica` returns false when `!ClientActive()`
   (`coop_time.cpp:497-499`), so vanilla `setDaytime` runs unchanged
   (the `#if TARGET_PC` block in `d_kankyo.cpp:1540` is excised for
   `TARGET_PC=0`). Verified.

6. **Pond fallback — vanilla 1×; special windows preserved.**
   `RatePerTick` (`coop_time_logic.h:158-178`): `kTimeRatePond2x` on a
   pond stage returns `0.036f` (daytime ≥300 or ≤60, triple), `0.024f`
   (150..195, double), `0.012f` (fallback, 1×). This matches vanilla
   `d_kankyo.cpp:1575-1584`: the outer `daytime += time_change_rate`
   (1×) plus the pond block's `+= rate; += rate` (300-60 → +2 = 3×) /
   `= daytime + rate` (150-195 → +1 = 2×) / nothing (else → 1×). The
   M3 fallback (`0.024f`, 2×) is corrected to `0.012f`. Selftest
   (`selftest_main.cpp:1395-1422`) covers both windows, the 0.012
   fallback at daytime 100/240, and the defensive non-pond rate-3 case.
   Verified.

7. **Docs — 00-network.md §5/§6/§8 match protocol.h v5.** §5 TimeSync
   row: "absolute phase `f32` 0..360 + day + rate + flags (v5)" ←
   `TimeSyncMsg{f32 time; u16 day; u8 rate; u8 flags}` (`protocol.h:404-410`).
   §5 TimeEvent row: "event id + time + day (v5)" ←
   `TimeEventMsg{u8 eventId; u8 pad; f32 time; u16 day}` (`protocol.h:415-421`).
   §5 WeatherChange row: "mode + thunder + intensity + colpat (v5)" ←
   `WeatherChangeMsg{u8 mode; u8 thunder; u16 intensity; u8 colpat; u8 pad}`
   (`protocol.h:426-432`). §4/§5 WorldInit rows now note "time + weather
   (v5)". §6 cadence note: "real since M3.5: a `NetClock::AtRate(1)` gate".
   §8 payload text updated. Verified.

8. **modelCallBack suppression — defense-in-depth, non-PC byte-identical,
   no break.** `d_a_alink.cpp:2427-2442`: the `#if TARGET_PC` block
   early-returns `true` for puppets (`dusk::coop::isPuppet(this)`); the
   rest (`jointControll`, `setUpperFront`, `setFootMatrix`, `setArmMatrix`,
   `changeBlendRate`) is unchanged. (a) Non-PC path: the `#if TARGET_PC`
   block is excised for `TARGET_PC=0` → byte-identical vanilla body. (b)
   Local Link: `isPuppet` (`coop.cpp:884-891`) returns false for the local
   Link — `EntryForPid(fopAcM_GetID(link))` returns nullptr (the local
   player is not in `g_puppets`, which holds remote puppets only), so the
   early-return doesn't fire and the full vanilla path runs. The
   early-return value (`true`) matches the function's normal exit. (c) No
   conflict with the debug-agent's envelope recompute (`8cd182d4d3`):
   that recompute runs in `ApplyPuppetState` (`coop.cpp:543-560`, the
   apply phase) and recomputes `calcWeightEnvelopeMtx()` from the pasted
   anm matrices; the `modelCallBack` suppression runs at draw time and
   skips the puppet's *own* per-joint procedural corrections (computed
   from its frozen local animation state) that would fight the pasted
   pose. They are complementary — both serve "puppet renders exactly the
   sender's pose" — and operate at different phases. The comment
   accurately names `8cd182d4d3` as the PRIMARY fix and this as
   defense-in-depth. Verified.

9. **Build & regression.** Forced `ninja -t clean dusklight` (2247 files)
   + `ninja -t clean dusk_net_selftest` → both rebuild clean and link
   clean (only pre-existing warnings). Selftest PASS incl. all M3 + M3.5
   checks. Diff scope is time/weather + docs + the M1-domain
   `modelCallBack` suppression — no M1/M2 behavioral change (protocol
   struct layout unchanged from v5; `coop_time_logic.h` is a pure
   refactor of decisions already in `coop_time.cpp`, now shared with the
   selftest). No M4 creep: no join-warp (`JoinAccept` stage still empty;
   no `dStage_changeScene`), no forms/wolf-revert, no host migration, no
   enemy/combat changes. `shutdown()` resets all statics
   (`coop_time.cpp:688-698`). Commit hygiene: 5 scoped commits, each
   with an accurate message referencing the M3 finding it fixes; the
   `AtRate` factory, the `SeedTargets` gate, the `NextThunderMode`
   policy, the pond fallback, the docs rows, and the `modelCallBack`
   suppression are each in the right commit.

10. **`onStageCreate` ordering comment + darkworld flag annotations.**
    The `coop_time.h:85-90` and `coop_time.cpp:562-574` comments now say
    "after the start-room layer resolution" and cite
    `dStage_Create → dStage_roomInit → getLayerNo → dKy_daynight_check`,
    then `dKankyo_create → dKy_Create → onStageCreate`. Verified against
    `d_stage.cpp:2758` (`dStage_roomInit`) → `d_stage.cpp:2763`
    (`dKankyo_create`) → `d_kankyo.cpp:8381` (`onStageCreate`). The
    darkworld flag bit is annotated in `protocol.h:195-199` ("bit 0")
    and `coop_time.cpp:242-246` ("informational today... carried for
    diagnostics and the future per-player forms subsystem (M4+)") —
    accurate per 04 §5.5.

---

## MUST-FIX before M4

**None.** M3.5 resolves MAJOR 1 and all MINORs from both M3 reviews. HEAD
builds clean (forced rebuild), the selftest is green (incl. the
non-tautological 1 Hz cadence regression guard and the table-driven
derive/thunder/pond checks), and M1/M2/M3 behavior is unchanged. MINOR 1
above is pre-existing host-side publish policy, out of M3.5's scope, and
not a regression — track it as a future polish (publish on thunder-bit
edge), not a gate for M4.
