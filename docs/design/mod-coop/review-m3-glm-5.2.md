# M3 adversarial review — time of day & weather sync (glm-5.2)

Review range: `149b4bc896..HEAD` (`c502743698`, `d14aeafb96`, `3a11d7ece0`).
Branch: `net-coop`. Scope: `docs/design/mod-coop/04-time-weather.md` mechanics on the
fork (`#if TARGET_PC`), wire contract `00-network.md §5/§8`, game-side replica in
`src/dusk/coop/coop_time.cpp`, routing in `src/dusk/net/session.cpp`, protocol in
`include/dusk/net/protocol.h` + `src/dusk/net/protocol.cpp`, and the `TARGET_PC`
attachment points in `src/d/d_kankyo.cpp` and `src/d/actor/d_a_kytag06.cpp`.

## VERDICT

**M3 meets its acceptance criteria, with one MAJOR spec/cadence bug to fix before M4.**

| Acceptance criterion (plan Rev 3 §5 M3) | Status | Evidence |
|---|---|---|
| Same sky on both | ✅ | Host publishes absolute phase (TimeSync) + mode/colpat (WeatherChange); clients pin + ramp with vanilla dice rules — `coop_time.cpp:RampRainTowardTarget/ThunderPerMode/DiceRainMinus` mirror `d_a_kytag06.cpp:51-65,199-235` exactly. Selftest confirms both clients receive WeatherChange. |
| Rain arrives together | ✅ | WeatherChange carries intensity; clients re-pin on receipt then ramp to the per-mode target (40/250) at the same ±1-3/frame rate as the host's dice machine. |
| Cutscene freeze = rate 0 on both | ✅ | `HostRate()`→`ClockFrozen()` returns `kTimeRateFrozen` under `event_runCheck`/msg-box/`GetTimePass()==0`/`using_time_control_tag`/`field_0x130a` (`coop_time.cpp:104-119`); client `AdvanceStep()` returns 0 for `kTimeRateFrozen` (`coop_time.cpp:236`). Host's own clock freezes via the vanilla gate (`d_kankyo.cpp:1546`). |
| Stage transitions re-assert | ✅ (caveat) | `onStageCreate()` re-asserts `dComIfGs_setTime/Date` + `PinWeather()` post-`dKy_Create` (`d_kankyo.cpp:8374`). Room-layer ordering is best-effort per 04 §5.10 — accepted v1. |
| Single-player untouched | ✅ | All hooks early-return on `!ClientActive()` (`sessionActive() && !hostRole()`); `#if TARGET_PC` guards are additive-only, no `#else` needed (non-PC path is excised → byte-identical to upstream). Verified: full game force-rebuild of the 4 touched TUs links clean; selftest 207/207 PASS. |

**Build/regression:** forced rebuild of `d_kankyo.cpp`, `d_a_kytag06.cpp`, `coop_time.cpp`,
`coop.cpp` (touched `21:08`, `.o` rebuilt `21:08`, linked into `Dusklight.app` — only
pre-existing vanilla warnings at `d_kankyo.cpp:3744/3904/4147/9541`, none in M3 code).
`dusk_net_selftest` → **207 ok, PASS** incl. the new `RunM3TimeWeatherCheck`
(TimeSync/TimeEvent/WeatherChange round-trips; host→all; rogue client→host consumed,
not relayed to B; JoinAccept carries host clock+sky; WorldInit roster-refresh).
M1/M2 TUs compile unchanged — the `TimeInfo`/`WeatherInfo`→`TimeStateInfo`/`WeatherStateInfo`
rename is clean (`grep` finds no stale `phase`/`elapsedMs`/`weatherId`/`timePhase` refs
outside protocol/session/coop_time/selftest).

---

## RANKED FINDINGS

### MAJOR 1 — TimeSync is published at 60 Hz, not the specified 1 Hz

**Evidence:** `src/dusk/coop/coop_time.cpp:65` declares `net::NetClock g_syncClock;` with
the comment `// 1 Hz TimeSync cadence (wall clock)`. But `NetClock` is a **60 Hz** clock
(`include/dusk/net/clock.h:21` `kRateHz = 60`, `kIntervalUs = 16667`). `PublishHostState`
runs every game frame and calls `g_syncClock.Tick(NowUs())` (`coop_time.cpp:268`), which
returns true every ~16.6 ms → a `TimeSync` is emitted **60×/s**, not 1×/s.

Every artifact describing this module says 1 Hz and is contradicted by the code:
- `include/dusk/net/protocol.h` `TimeSyncMsg` doc: "Unreliable-sequenced, 1 Hz."
- `include/dusk/coop/coop_time.h:30`: "publishes TimeSync (1 Hz + on stage/rate change)"
- `00-network.md:136,236,274`: "TimeSync … 1 Hz"
- commit `d14aeafb96` body: "1 Hz TimeSync (wall-clock NetClock)"
- the local variable is literally named `oneSecondDue` (`coop_time.cpp:268`).

The selftest does not check cadence, so this slipped past it.

**Impact:** functionally benign on LAN (unreliable-sequenced, freshest-wins, absolute
phase — 8 B × 60 = 480 B/s/client ≈ 3.8 KB/s worst case for 8 clients), but it is a
spec violation, 60× the documented bandwidth, and the `1 Hz` assumption is baked into
the docs/commit/comments. Any future consumer that rate-limits or assumes 1 Hz would
break. Also makes the "roster-refresh can never regress a fresher TimeSync" reasoning
in `onGameFrame` (MINOR 1) load-bearing where it otherwise wouldn't be.

**Fix:** either count 60 `Tick`s per send, or give `NetClock` a `kRateHz` ctor arg and
instantiate `g_syncClock` with 1 Hz, e.g.:
```cpp
net::NetClock g_syncClock;        // today: 60 Hz
// →
static net::NetClock g_sync1Hz(0); // then Tick at 1 s: needs a 1 Hz clock
```
Simplest: add `NetClock(u32 hz)` and use `g_sync1Hz(1)`, or gate the send on
`g_syncClock.Frame() % 60 == 0`. Verify the selftest still passes and add a cadence
assert (e.g. over 200 ms, expect ≤ 4 TimeSyncs).

---

### MINOR 1 — `onGameFrame` re-seed can transiently regress a fresher TimeSync on a 3rd-player join

**Evidence:** `coop_time.cpp:onGameFrame` (client branch) re-seeds `g_time`/`g_weather`
from `coop::worldTime()`/`worldWeather()` whenever the session world state differs from
`g_worldSeen*`. The session world state on a client changes only on `JoinAccept` and on
the `WorldInit` roster-refresh re-broadcast (sent to existing peers on every later join,
`session.cpp:424`). The re-broadcast carries the host's *current* time. Because
`TimeSync` is unreliable-sequenced and `WorldInit` is reliable, a `TimeSync(time=T+a)`
sent after the `WorldInit(time=T)` can be delivered **before** the reliable `WorldInit`
(head-of-line blocking on the reliable channel). The client then has `g_time.time = T+a`,
then the `WorldInit` arrives, `SeedTargets()` runs (T ≠ `g_worldSeen.time`), and
**regresses** `g_time.time` back to `T` until the next `TimeSync` re-advances it.

The header comment claims this "can never regress fresher TimeSync/WeatherChange
receipts" (`coop_time.cpp:48-50`); that is true only for re-broadcasts of the *same*
world state (the `g_worldSeen` guard), not for a `WorldInit` carrying *newer* state
than a `TimeSync` that raced ahead of it.

**Impact:** transient (one packet), self-correcting, and only on joins; with the
current 60 Hz `TimeSync` (MAJOR 1) the regression window is ≤ 16 ms. **If MAJOR 1 is
fixed to 1 Hz**, this becomes up to ~1 s of held-back time visible on every 3rd+ join.
So MAJOR 1 and MINOR 1 interact — fix MAJOR 1 first, then either drop the re-seed
of `g_time.time` from `SeedTargets` (keep only `g_weather` re-seed, since weather has
no "fresher" per-frame stream), or skip the `g_time` re-seed when a `TimeSync` was
received more recently than the last `WorldInit`.

---

### MINOR 2 — `00-network.md §5/§8` wire-layout descriptions are stale vs the v5 code

**Evidence:** `docs/design/mod-coop/00-network.md`:
- line 136: `| TimeSync | unreliable seq | 1 Hz | time phase (u32) + elapsed delta |`
- line 138: `| WeatherChange | reliable | on change | weather id + intensity |`
- line 274: `TimeSync (phase + delta, 1 Hz, unreliable)`

The v5 wire format (`include/dusk/net/protocol.h`, `src/dusk/net/protocol.cpp`) is:
`TimeSync{f32 time, u16 day, u8 rate, u8 flags}` (8 B) and
`WeatherChange{u8 mode, u8 thunder, u16 intensity, u8 colpat, u8 pad}` (6 B).
`kProtocolVersion` was correctly bumped 4→5 (`protocol.h:43`) and `protocol.h`'s own
v5 changelog describes the new layout, but the `00-network.md` table that the review
contract points at was not updated.

**Fix:** update `00-network.md:136-138` and `:274` to the v5 fields (or add a one-line
"see protocol.h §TimeSyncMsg for the v5 absolute-phase layout" pointer). Normative
reference is already `protocol.h` per its own header comment, so this is doc-only.

---

### MINOR 3 — darkworld branch of `clientClockReplica` does not mirror vanilla's `using_time_control_tag` gate

**Evidence:** vanilla `setDaytime` (`d_kankyo.cpp:1544`) wraps *both* the day and the
twilight (`else` of `!dKy_darkworld_check()`, `d_kankyo.cpp:1580`) branches in
`if (using_time_control_tag == 0)`. The replica's darkworld branch
(`coop_time.cpp:clientClockReplica`, the `if (dKy_darkworld_check())` arm) advances
`env.dark_daytime += env.time_change_rate` **unconditionally** — it does not test
`using_time_control_tag`. So when a kytag11 time-control tag is active in a twilight
stage on a synced client, the client's `dark_daytime` advances while the host's
(under the same tag) would hold.

**Impact:** niche (twilight + active time-control tag co-occurrence). The host's
`HostRate()` returns `kTimeRateFrozen` for darkworld and sets `kTimeFlagDarkworld`,
so the *day* clock is correctly frozen on both sides; only the local twilight
`dark_daytime` differs, and twilight lighting is fixed (not driven by `dark_daytime`
phase in a way that would desync the shared sky). Per 04 §5.5 the twilight clock is
local story, so this is cosmetic. Still, the doc says "keep vanilla darkworld
semantics" and the vanilla gate is `using_time_control_tag == 0`.

**Fix:** wrap the darkworld advance in `if (env.using_time_control_tag == 0)`.

---

## VERIFIED-OK

1. **Wire layouts match 04 §4 / protocol.h.** `TimeSyncMsg`/`TimeEventMsg`/`WeatherChangeMsg`
   (`protocol.h:273-296`) match the serializers/deserializers field-for-field
   (`protocol.cpp:Serialize/Deserialize` for the three types; `WireSize` returns 8/8/6).
   `JoinAcceptMsg`/`WorldInitMsg` carry `TimeStateInfo`+`WeatherStateInfo`
   (`protocol.h:JoinAcceptMsg/WorldInitMsg`); serializers write time+weather for both
   (`protocol.cpp:SerializeJoinAccept/WorldInit`). `kProtocolVersion` bumped 4→5 with
   a changelog entry describing the v5 absolute-phase contract. ✅
2. **Relay policy: host→all only; client-sent time/weather consumed-and-not-relayed.**
   `PolicyFor(TimeSync/TimeEvent/WeatherChange) = RelayPolicy::None` (`session.cpp:57-66`);
   host `SendGameMessage` → `SendToAll`; client `SendGameMessage` → `SendToPeer(0)`;
   host `HandleData` for the three types calls `gameHandler_` only and never
   `ForwardGameMessage`/`SendToAll` (`session.cpp:321-332`). Selftest asserts B does not
   receive A's rogue TimeSync/WeatherChange. ✅
3. **Time+weather in JoinAccept/WorldInit.** `OnJoinRequest` fills `accept.time`/`accept.weather`
   and `init.time`/`init.weather` from `worldTime_`/`worldWeather_` (`session.cpp:401-414`);
   `OnWorldInit` stores them on the client (`session.cpp:499-500`). Host `PublishHostState`
   refreshes `setWorldTime/Weather` every frame so joiners get current values. Selftest
   asserts JoinAccept carries host clock+sky. ✅
4. **Rate model (host).** `ClockFrozen()` mirrors the vanilla `setDaytime` advance gate
   exactly (`using_time_control_tag`, `field_0x130a`, `event_runCheck`, msg-box `mode>=2`,
   `roomControl_getTimePass`) → `kTimeRateFrozen`; `==1.0`→`kTimeRateFast`; `F_SP127/R_SP127`
   →`kTimeRatePond2x`; `>=1000`→Frozen (debug/menu skip). Darkworld→Frozen+flag.
   (`coop_time.cpp:ClockFrozen/HostRate`.) The wolf-howl 90/285 slowdown is the fork's own
   `#if TARGET_PC` patch in `setDaytime` (`d_kankyo.cpp:1556-1566`) — host runs it natively;
   the rate-change to Normal triggers an immediate `TimeSync` (`rate != g_lastRate`,
   `coop_time.cpp:289`), so the client switches off fast-forward within one frame of the
   host detecting the slowdown. ✅
5. **Client clock replica.** `clientClockReplica` returns false when `!ClientActive()`
   (net off / host → vanilla `setDaytime` runs unchanged); otherwise advances by
   `AdvanceStep()` (= `ratePerTick × 1 sim tick`, since `setDaytime` is once per sim tick),
   wraps 360 → `mDate++` + local `dKankyo_DayProc` equivalent
   (`dComIfGs_offTmpBit(tempBitLabels[91])` — verified identical to
   `d_kankyo_static.h:14`), keeps the audio clock (`mDoAud_setHour/Minute/Weekday`) and
   save writes and `using_time_control_tag=0` reset in vanilla's exact tail order
   (`coop_time.cpp:clientClockReplica` vs `d_kankyo.cpp:1666-1675`). Absolute phase
   adopted on `TimeSync` receipt (`onGameMessage`). Darkworld branch runs the local
   `dark_daytime` clock and forces `daytime=0` per 04 §5.5. ✅ (modulo MINOR 3)
6. **`#if TARGET_PC` guard hygiene.** All four attachment points
   (`setDaytime`, `dKy_Execute`, `dKy_Create` in `d_kankyo.cpp`; `daKytag06_Draw` in
   `d_a_kytag06.cpp`) are additive `#if TARGET_PC … #endif` blocks with **no `#else`** —
   the non-PC path is the block simply excised, byte-identical to upstream. The
   `d_a_kytag06.cpp` guard wraps only the `daKytag06_type_04_Execute` call in
   `if (!suppressDiceWeather()) { … }` — with `TARGET_PC=0` this collapses to
   `{ daKytag06_type_04_Execute(i_this); }`, semantically identical to vanilla. ✅
7. **Dice suppression (R16).** `suppressDiceWeather()` returns `ClientActive()` only
   (host/offline run the dice machine). The type-4 branch (`daKytag06_type_04_Execute`
   → `dKy_event_proc`-inlined tables) drives **weather state only** — `raincnt`,
   `mColpatWeather`/`mColpatCurrGather`, `mThunderEff.mMode`, `dice_wether_*` counters
   (`d_a_kytag06.cpp:192-256`); no non-weather side effects. Suppressing the whole
   call on synced clients is safe. kytag00 shelter tags are NOT suppressed — they run
   locally and modulate the synced base (cap/restore `raincnt` from `base_raincnt`,
   which the client's `RampRainTowardTarget` sets via `dKyw_rain_set` exactly as the
   host's dice does). ✅
8. **Client weather ramp = vanilla dice rules.** `DiceRainMinus` (`coop_time.cpp:147`)
   is byte-identical to `dice_rain_minus` (`d_a_kytag06.cpp:51`): `g_Counter.mCounter0 & 3`,
   `>40 -=3`, else `--`. `RampRainTowardTarget` mode targets (RainLight→40 ±1,
   RainHeavy/ThunderHeavy→250 +1, else drain) match `d_a_kytag06.cpp:206-235`.
   `ThunderPerMode` (Thunder→1, Cloudy→0, Clear: `==1→0`, Rain/Snow: leave) matches the
   dice proc's `mThunderEff.mMode` writes. `dKyw_rain_set` sets `base_raincnt` on both
   sides identically. ✅
9. **Stage-change re-assert ordering (04 §5.6/§5.10).** `onStageCreate` (post-`dKy_Create`)
   re-asserts `dComIfGs_setTime/Date` + `PinWeather` before the first `dKyw_wether_move`
   of the new stage; `dKy_daynight_check` reads `dComIfGs_getTime()` (`d_kankyo.cpp:1757`),
   so the time re-assert is effective for `dComIfG_get_timelayer`. Room-layer resolution
   happens at stage/room load; if it runs before `dKy_Create` the re-assert is one stage
   late — explicitly accepted v1 (04 §5.10 "worst case one stale layer"). ✅ (caveat
   acknowledged in code comment)
10. **Edge cases handled in code (04 §5.1-§5.16):**
    - 5.1 stage-transition time reset → `onStageCreate` + `PublishHostState` stage-change
      re-seeds `g_lastMode=0xFF`/`g_stageSeeded=false` and sends immediate TimeSync+
      WeatherChange (`coop_time.cpp:255-262,289-297`). ✅
    - 5.2 save/load → host keeps its save; client's per-frame replica overrides any
      local save time. ✅
    - 5.3 event freeze → `ClockFrozen` → `rate=0`. ✅
    - 5.4 wolf-howl → `rate=Fast`, boundary TimeEvent + rate-change immediate TimeSync. ✅
    - 5.5 twilight → host `flags|=kTimeFlagDarkworld`, `rate=Frozen`; client runs local
      `dark_daytime` when its own `dKy_darkworld_check()`, else forces synced time. ✅
      (MINOR 3)
    - 5.6 weather reset per stage → `onStageCreate`/`PinWeather` + host re-sends
      WeatherChange on `stageChanged`. ✅
    - 5.7 dice stages → `suppressDiceWeather` + `IsDiceStage` derivation. ✅
    - 5.8 daybreak wolf-revert → **deferred to M4+** (no code; DAWN/DUSK `TimeEvent`
      handling is a no-op in `onGameMessage`, correctly labeled "deferred"). ✅ not
      implemented, not creeped.
    - 5.10/5.11/5.12/5.14/5.15 → covered (layer best-effort; pond 2x segment rule in
      `AdvanceStep`; debug ≥1000 hold-then-snap; absolute phase/day no drift; event
      weather overridden by per-frame force). ✅
11. **No M4 creep.** No join-warp/unlock-gate code, no forms/wolf-revert, no host-leave
    UX, no LAN discovery — all deferred per plan §5 M4. The diff touches only the listed
    M3 files. `D8` (host migration) and `D6` (join warp) are not implemented. ✅
12. **No dead code.** Every module-static (`g_time`, `g_weather`, `g_clientSeeded`,
    `g_worldSeen*`, `g_syncClock`, `g_lastStage/Phase/Day/Rate/Mode/Intensity`,
    `g_stageSeeded`) and every helper (`ClockFrozen`, `HostRate`, `IsDiceStage`,
    `DeriveWeather`, `ClampU16`, `AdvanceStep`, `DiceRainMinus`, `RampRainTowardTarget`,
    `PinWeather`, `ThunderPerMode`, `SnowPerFrame`, `SeedTargets`, `ClientActive`,
    `NowUs`, `SendMessage`, `Send*`) is referenced. `shutdown()` resets all module state. ✅
13. **Commit hygiene.** Three scoped commits (routing / module+dice / kankyo replica),
    each building. `files.cmake` adds `coop_time.{h,cpp}`. Commit messages describe
    intent accurately (modulo the "1 Hz NetClock" mislabel that is MAJOR 1). ✅

---

## MUST-FIX before M4

1. **MAJOR 1** — make `TimeSync` actually 1 Hz (the `NetClock` is 60 Hz; the variable is
   even named `oneSecondDue`). Add a cadence assert to the selftest.
2. **MINOR 1** — after fixing MAJOR 1, re-examine the `onGameFrame` re-seed of `g_time`
   from `WorldInit` on roster-refresh; at 1 Hz a `TimeSync` that races ahead of the
   reliable `WorldInit` can regress the client clock by up to ~1 s on every 3rd+ join.
   Either drop the `g_time.time` re-seed in `SeedTargets` (keep weather) or gate on
   "last `TimeSync` newer than last `WorldInit`".

## NICE-TO-FIX (non-blocking)

- **MINOR 2** — update `00-network.md §5/§8` TimeSync/WeatherChange rows to the v5
  fields (or point at `protocol.h`).
- **MINOR 3** — gate the `clientClockReplica` darkworld advance on
  `using_time_control_tag == 0` to mirror vanilla exactly.
- Add a selftest cadence check (count `TimeSync`s over a window) so MAJOR 1-class
  regressions are caught.
- The `TimeEvent`/`WeatherChange` `pad`/reserved bytes are not zero-validated on
  deserialize (`protocol.cpp` reads them but the selftest only checks the meaningful
  fields) — harmless, but a strict version-gate could reject nonzero reserved bytes.
