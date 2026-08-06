# Time of day & weather sync (investigation 04)

Status: **Investigation — how a stock-Dusklight `.dusk` mod reads, controls, and syncs
time-of-day and weather.** Feeds `docs/design/network.md` §9 and `docs/design/mod-coop/00-network.md` §8.

Authority model (from the design): the **world host owns one clock and one sky**. Host runs the
vanilla simulation unchanged; clients replicate. Everything below is about the exact state,
the frame order, and the hook points that make that possible on a **stock, unmodified** Dusklight.

---

## 1. Time-of-day state

### 1.1 Where it lives

Two copies, kept in sync by the kankyo process:

| Copy | Location | Type | Meaning |
|------|----------|------|---------|
| Persistent | `g_dComIfG_gameInfo.info.getPlayer().getPlayerStatusB()` — `dSv_player_status_b_c::mTime` @ **0x0C**, `mDate` @ **0x10** (`include/d/d_save.h:186-209`) | `BE(f32)`, `BE(u16)` | time 0..360, day counter. This is the save-truth. |
| Working | `g_env_light` (`dScnKy_env_light_c` global, `include/d/d_kankyo.h:441`, symbol `_g_env_light`) — `daytime` @ **0x1244**, `mDate` @ **0x12BE** | `f32`, `u16` | per-frame working copy, re-read from save at the top of every `setDaytime()`. |

Accessors (all `inline` in `include/d/d_com_inf_game.h:1503-1510`, usable directly from a mod):
`dComIfGs_getTime()`, `dComIfGs_setTime(f32)`, `dComIfGs_getDate()`, `dComIfGs_setDate(u16)`.

Units: **degrees of a 360° day, 15 units = 1 hour** (`dKy_getdaytime_hour() = time / 15`).
Conventions used across the engine:

| Event | time | hour |
|-------|------|------|
| Midnight / day rollover | 0 (or 360 wrap) | 0 |
| **Dawn** (`dKy_daynight_check` flips to day) | **90** | 6 |
| **Dusk** (flips to night) | **285** | 19 |
| Night in kytag06 type-7 wind logic | `> 285 || < 82.5` | — |

`dKy_daynight_check()` (`src/d/d_kankyo.cpp:1747`, symbol `__Z18dKy_daynight_checkv`) is the
single day/night predicate the whole engine uses: hour ∈ [6, 19) → day.

### 1.2 How time advances

`dScnKy_env_light_c::setDaytime()` (`src/d/d_kankyo.cpp:1531-1666`, symbol
`_ZN18dScnKy_env_light_c10setDaytimeEv`) — runs once per sim tick from `exeKankyo()`:

1. `mDate = dComIfGs_getDate(); daytime = dComIfGs_getTime();`
2. Advance **only if all** of:
   - `using_time_control_tag == 0` (kytag11 time-control tag is active elsewhere),
   - not darkworld (`!dKy_darkworld_check()` — twilight runs its own `dark_daytime` clock),
   - `dComIfGp_event_runCheck() == FALSE` (**time frozen during events/cutscenes**),
   - message box mode < 2 (dialogue open),
   - `dComIfGp_roomControl_getTimePass()` (**per-room flag from the stage file**;
     `dStage_roomControl_c::m_time_pass`, set from room data `field_0x2 & 3` on stage/room load,
     `src/d/d_stage.cpp:1512,2838` — this is why dungeons/indoors don't pass time),
   - `!field_0x130a` (fishing/shop scene flag, set by the Shaman actor).
3. `daytime += time_change_rate` (default **0.012 per sim tick** ≈ 16.7 min real-time per day at
   30 Hz sim; fixed per-sim-tick, not wall-clock — same on every machine).
4. Wrap: `if ((u32)daytime >= 360) { daytime = 0; mDate++; dKankyo_DayProc(); }`
   (`dKankyo_DayProc` is a static weak in `include/d/d_kankyo_static.h` that clears temp bit 91 —
   Ordon Ranch day-1 story flag; trivially replicated by a mod via
   `dComIfGs_offTmpBit(dSv_event_tmp_flag_c::tempBitLabels[91])`).
5. `dComIfGs_setTime(daytime)`, then `mDoAud_setHour/Minute/Weekday(...)` (ambient audio clock),
   `dComIfGs_setDate(mDate)`, `using_time_control_tag = 0`.

`time_change_rate` (`g_env_light` @ **0x124C**):

| Value | Meaning | Who sets it |
|-------|---------|-------------|
| `0.012` | normal | stage init (`dKy_Create`) |
| `1.0` | fast-forward to dawn/dusk (wolf-howl skip; `setDaytime`'s `#if TARGET_PC` block drops it back to 0.012 on crossing 90/285) | `d_a_alink_wolf.inc:4211` |
| `1000+` | DEBUG / save-menu time-pick markers (stage init skips its own time set when ≥ 1000) | `d_s_menu.cpp:1443-1458` |

### 1.3 Stage-transition time policy (critical for sync)

`dKy_Create` (`src/d/d_kankyo.cpp:8297`, static, symbol `__ZL10dKy_CreatePv`) runs on every
stage load and calls `envcolor_init()` (`src/d/d_kankyo.cpp:1242`, static) which **overwrites the
save time**:

1. If the stage's `stagInfo` has a start hour (`dStage_stagInfo_GetTimeH` ≥ 0) →
   `dComIfGs_setTime(hour * 15)`. Dungeon stages use ≥ 128 (interpreted negative → skipped).
2. If `g_env_light.nexttime != -1` → `dComIfGs_setTime(nexttime)` (transition carry wins).
   `dStage_changeScene` / `dStage_changeScene4Event` set `nexttime` from the exit's scene-list
   `timeH` when `timeH < 31` (`src/d/d_stage.cpp:2884,2938`); `timeH == 31` means "keep current".
   `dKy_set_nexttime(f32)` (symbol `__Z16dKy_set_nexttimef`) is the setter.
3. Twilight bookkeeping: on entering darkworld `old_time` (@ 0x1274) captures the time; on leaving
   it restores it.
4. `time_change_rate` reset to 0.012 (unless a DEBUG/menu skip marker ≥ 1000 is set); weather
   fully reset (see §3).

**Net effect:** time NEVER survives a stage transition by itself — it is either the stage's default
hour or the transition's carried time. A synced client must re-assert host time after every stage
load.

Explicit time changes in events: `dKy_instant_timechg(f32 time)` (symbol `__Z19dKy_instant_timechgf`)
— if `time < current`, increments date + calls `dKankyo_DayProc()` (midnight crossed), then
`dComIfGs_setTime(time)`. The cutscene staff "LIGHT" (`dEvDtStaff_c::specialProcLight`,
`src/d/d_event_data.cpp:495-545`) uses exactly this for `CHAN hour` / `ADD_ hour` staff
commands — this is the vanilla "time change" API surface.

### 1.4 Consumers that branch on time (all read the same state → come along for free)

- Lighting: `CalcTevColor()` (palette schedule via `setLight_palno_get`, blends by
  `pat_ratio`), `setSunpos()` (sun/moon positions), `SetBaseLight()`.
- Sky: sun/moon packet visibility, **star density** (`wether_move_star` ramps by
  `getDaytime()`), kytag15 "z-shake" night overlay.
- Spawns: `d_a_e_hp.cpp` (ghost/poe) `mNight && !dKy_daynight_check()`; NPC schedules
  (`d_a_npc_passer2.h` reads `dKy_getdaytime_hour/Minute`).
- **Room layers**: `dComIfG_get_timelayer()` (`src/d/d_com_inf_game.cpp:158`) adds +1 layer at
  night for certain stages (Kakariko, Castle Town, Lake Hylia, …). Layers are resolved at
  stage/room load — crossing dusk/dawn does **not** hot-swap geometry.

---

## 2. Weather state

All fields on `g_env_light` (`include/d/d_kankyo.h`). Weather is fully reset per stage by
`dKyw_wether_init()` (symbol `__Z16dKyw_wether_initv`), called from the `dKyeff` process create
(`src/d/d_kyeff.cpp:117`).

### 2.1 State variables

| Field | Offset | Meaning |
|-------|--------|---------|
| `raincnt` | 0x0E80 `int` | **rain intensity 0..250** — the master knob. >3 spawns the rain packet, ≥125 heavy-rain audio, ≤3 tears it down (`wether_move_rain`). `dKy_rain_check()` returns it. |
| `base_raincnt` | 0x129C `int` | shelter-restore value; `dKyw_rain_set(count)` sets both. kytag00 (indoor/shelter tags) caps `raincnt` at it and restores from it. |
| `mSnowCount` | 0x0E8C `int` | snow intensity 0..500 (Snowpeak stages). |
| `mThunderEff.mMode` | 0x0ED4 `EF_THUNDER` | 0/1 = lightning on/off (mStatus = packet lifecycle). |
| `mColpatWeather` | 0x12C8 `u8` | **color-palette weather id: 0 clear, 1 cloudy/light-rain, 2 heavy/storm.** Drives lighting. |
| `wether_pat0` / `wether_pat1` | 0x12C2 / 0x12C3 `u8` | palette blend bookkeeping (prev/curr), consumed by `exeKankyo`'s gather logic. |
| `wether` | 0x12CC `u8` | stage-file weather id — only meaningful in special stages (Fishing Pond F_SP127/R_SP127, D_MN07A) and boss arenas (kytag06 type 6/7 read it); reset to 0 everywhere else at stage init (`d_kankyo.cpp:1348-1357`). |
| `dice_wether_mode` | 0x12C9 `u8` | rolling-weather state machine mode: `DICE_MODE_SUNNY..THUNDER_HEAVY (0..5), UNK6=6` (enum in `d_kankyo.h:972`). |
| `dice_wether_state` / `dice_wether_pat` / `dice_wether_counter` / `dice_wether_time` / `dice_wether_change_time` | 0x12CA / 0x12CB / 0x1298 / 0x11D8 / 0x11D4 | the rest of the state machine. |
| `mMoyaMode` / `mMoyaCount` (0x0EB5/0x0EB8), `mHousiCount` (0x0EAC), `mStarCount`/`mStarDensity` (0x0E9C/0x0EA0), `mCloudInitialized` (0x0EB4), `mEvilInitialized` (0x1050) | — | secondary effects; derive from the above + stage (housi/moya are stage-specific ambience, stars derive from time). |

### 2.2 How weather changes

**Rolling weather (overworld).** The "dice weather" state machine lives in `dKy_event_proc`
(`src/d/actor/d_a_kytag06.cpp`, kytag06 type 4), invoked from `daKytag06_Draw`
(local symbol `__ZL14dDaKytag06_DrawP13kytag06_class` — note the mangled form starts
`__ZL14daKytag06_Draw...`; `dKy_event_proc` itself is inlined away, only its static tables remain
in the symbol table). It runs only in stages that place kytag06 type-4 actors — Faron Woods
(F_SP108), Fishing Pond (F_SP127), Hyrule Field (F_SP121) — and only when
`!camera_water_in_status && light_init_timer == 0`. It rolls a pattern table (e.g. sunny →
light rain → sunny, each 7.5-30 time-units), then **ramps `raincnt` ±1-3 per frame toward the
mode's target** (RAIN_LIGHT ≈ 40, RAIN_HEAVY ≈ 250) and sets `mColpatWeather` +
`mThunderEff.mMode` per mode (SUNNY→colpat 0, CLOUDY→1, RAIN_LIGHT→1, RAIN_HEAVY→2,
THUNDER_LIGHT→1+thunder, THUNDER_HEAVY→2+thunder). Colpat application is gated on
`mColPatMode == 0 && mColPatModeGather == 0` (no forced color-mode event).

**Boss-arena weather.** kytag06 types 6/7 read `env_light->wether` (stage-file id) and run
`daKytag06_wether_proc` (rain/snow/thunder ramps + `dKy_change_colpat`).

**Event weather.** Cutscene staff "LIGHT" `Weather 0` → `dKy_instant_rainchg()`
(symbol `__Z19dKy_instant_rainchgv`): `raincnt = 250`, `mColpatWeather = 1`, `wether_pat0 = 1`,
`wether_pat1 = 1` (instant, no blend).

**Shelter.** kytag00 (`src/d/actor/d_a_kytag00.cpp`) — per-room weather-area tags: cap/restore
`raincnt` from `base_raincnt`, zero `mSnowCount`/`mMoyaCount`/`mHousiCount`/thunder, and blend
color patterns via `mColpatPrevGather/CurrGather` + `mColPatModeGather` (this is why the dice
proc checks `mColPatModeGather == 0`).

Per-frame driver: `dKyw_wether_move()` (symbol `__Z16dKyw_wether_movev`, called from
`dKyeff_c::execute`, `d_kyeff.cpp:65`) spawns/updates/tears down the rain/snow/sun/star/housi/
moya/mud/evil packets from the fields above; `dKyw_wether_draw()` (from `dKyeff_Draw`) renders
them. `dKyw_wether_proc()` (symbol `__Z16dKyw_wether_procv`, called inside `exeKankyo`) only
handles mist/wind triggers.

---

## 3. Frame order (where a mod hooks)

One sim tick, in order:

1. **`dKy_Execute`** (static, `__ZL11dKy_ExecuteP17sub_kankyo__class`) → `exeKankyo()`
   (symbol `_ZN18dScnKy_env_light_c9exeKankyoEv`):
   `mColPat` gather logic → **`setDaytime()`** (clock advance) → `dKyw_wether_proc()` →
   **`CalcTevColor()`** (lighting from `daytime`, `pat_ratio`, `wether_pat0/1`) → Sndpos →
   Eflight_flush.
2. **`dKyeff::execute`** (`d_kyeff.cpp:65`) → `dKyw_wether_move()` (+ move_draw, senses, env SE).
3. **Actor draws** — incl. kytag06 `daKytag06_Draw` → **dice weather proc** (overworld stages),
   and kytag00 area tags, which mutate `raincnt`/`colpat` *after* step 2 (one-frame latency in
   vanilla).
4. **`dKy_Draw`** (static, `__ZL8dKy_DrawP17sub_kankyo__class`) → `drawKankyo()` →
   `setSunpos()` (sun/moon positions from `g_env_light.daytime`), `SetBaseLight`, `setLight`.

Consequence for a mod:

- **Time**: replace `setDaytime()` on clients → everything downstream in the same tick
  (lighting, sunpos, star density, audio) reads the forced value. Nothing else reads time
  directly except kytag06-type-7 wind (reads `dComIfGs_getTime()` — same value).
- **Weather**: force `raincnt`/`mSnowCount`/`mThunderEff.mMode`/`mColpatWeather` in a **pre-hook
  of `exeKankyo`** so `CalcTevColor` sees the synced colpat the same tick; suppress the local dice
  machine (hook `daKytag06_Draw`, skip when `mType == 4`) so it can't fight the sync; keep
  kytag00 shelter tags running (they locally modulate the synced *base*, exactly as on the host).

### Verified hookable symbols (nm on the stock build)

| Display name | Mangled (Mach-O) | Kind |
|--------------|------------------|------|
| `dScnKy_env_light_c::setDaytime` | `_ZN18dScnKy_env_light_c10setDaytimeEv` | exported |
| `dScnKy_env_light_c::exeKankyo` | `_ZN18dScnKy_env_light_c9exeKankyoEv` | exported |
| `dScnKy_env_light_c::getDaytime` | `_ZN18dScnKy_env_light_c10getDaytimeEv` | exported |
| `dKy_instant_timechg(f32)` | `__Z19dKy_instant_timechgf` | exported |
| `dKy_instant_rainchg()` | `__Z19dKy_instant_rainchgv` | exported |
| `dKy_set_nexttime(f32)` | `__Z16dKy_set_nexttimef` | exported |
| `dKyw_rain_set(int)` | `__Z13dKyw_rain_seti` | exported |
| `dKyw_wether_move()` | `__Z16dKyw_wether_movev` | exported |
| `dKyw_wether_proc()` | `__Z16dKyw_wether_procv` | exported |
| `dKy_daynight_check()` | `__Z18dKy_daynight_checkv` | exported |
| `dKy_darkworld_check()` | `__Z19dKy_darkworld_checkv` | exported |
| `dKy_change_colpat(u8)` | `__Z17dKy_change_colpath` | exported |
| `dKy_Create` / `dKy_Execute` / `dKy_Draw` | `__ZL10dKy_CreatePv` etc. | local, hookable by name |
| `daKytag06_Draw` / `daKytag06_Execute` | `__ZL14daKytag06_DrawP13kytag06_class` etc. | local, hookable by name |
| `g_env_light` / `g_dComIfG_gameInfo` | `_g_env_light` / `_g_dComIfG_gameInfo` | data |

`dKankyo_DayProc`, `dKy_event_proc`, `dice_wether_*`, `wether_move_*` are **not** addressable
(static/inlined) — replicate their behavior instead of hooking them.

---

## 4. Proposed sync message schema

Aligns with `00-network.md` §5/§8 (`TimeSync`, `TimeEvent`, `WeatherChange`). Fixed-size LE
structs, hand-written serializers.

### 4.1 TimeSync (unreliable-sequenced, 1 Hz, absolute)

```c
struct TimeSync {
    u16 type;      // TIME_SYNC
    f32 time;      // phase 0..360 (IEEE f32, bit-identical across machines) — absolute, self-correcting
    u16 day;       // dComIfGs_getDate()
    u8  rate;      // 0 frozen | 1 normal (0.012/tick) | 2 fast (1.0) | 3 fishing-pond 2x
    u8  flags;     // bit0: darkworld (host's twilight clock is active; see §5.5)
};
// 12 bytes
```

Clients adopt the absolute phase/day on receipt, then advance their local replica by
`rate * simTicksSinceLastSync` (they count sim ticks — each `exeKankyo` invocation is one).
A lost packet self-corrects on the next one. Rate transitions (wolf-howl skip, kytag11 stages,
events ending) are carried by `rate`; a rate change also triggers an immediate TimeSync.

### 4.2 TimeEvent (reliable, on boundary crossing)

```c
struct TimeEvent {
    u16 type;      // TIME_EVENT
    u8  event_id;  // NEW_DAY=0 | DAWN=1 | DUSK=2
    u8  pad;
    f32 time;      // phase at event
    u16 day;
};
// 12 bytes (8 payload + 4 envelope; capstone MINOR I corrected the stale 10)
```

- **NEW_DAY**: host clock wrapped 360 → `mDate++` (host fires its own `dKankyo_DayProc`).
- **DAWN / DUSK**: crossing **time 90 / 285** (`dKy_daynight_check` boundary).
- Client handling: NEW_DAY → run the local `dKankyo_DayProc()` equivalent (temp bit 91 — local
  story flag) and bump day; DAWN/DUSK → hook for per-player logic (see §5.8 — the planned
  daybreak wolf-revert composes here via the `forms` subsystem) and any local one-shots.
- These events are advisory: the phase in TimeSync already encodes the boundary; the reliable
  event exists so local *one-shot* behavior fires exactly once, and for future form logic.

### 4.3 WeatherChange (reliable, on change)

```c
struct WeatherChange {
    u16 type;      // WEATHER_CHANGE
    u8  mode;      // semantic id — see table
    u8  thunder;   // mThunderEff.mMode (0/1)
    u16 intensity; // raincnt 0..250 (rain) or mSnowCount 0..500 (snow stages)
    u8  colpat;    // 0 clear | 1 cloudy/light | 2 heavy/storm (mColpatWeather)
    u8  pad;
};
// 10 bytes
```

`mode` (semantic): `CLEAR, CLOUDY, RAIN_LIGHT, RAIN_HEAVY, THUNDER_LIGHT, THUNDER_HEAVY, SNOW`.
Derivation on the host (`d_kankyo.cpp` + kytag06): mode → (colpat, thunder, target raincnt):

| mode | colpat | thunder | target raincnt |
|------|--------|---------|----------------|
| CLEAR | 0 | 0 | 0 |
| CLOUDY | 1 | 0 | 0 |
| RAIN_LIGHT | 1 | 0 | ~40 |
| RAIN_HEAVY | 2 | 0 | 250 |
| THUNDER_LIGHT | 1 | 1 | 0 |
| THUNDER_HEAVY | 2 | 1 | 250 |
| SNOW | 1 | 0 | — (snow count) |

**Throttle policy**: sync on *mode* change (rare — patterns last minutes) and carry the current
intensity with it. Do **not** stream the ±1-3/frame ramp — clients re-run the vanilla ramp toward
the target (same rules as `dKy_event_proc`), so visuals match without message spam. Re-send on
stage change (weather is fully reset per stage) and in `JoinAccept`/`WorldInit` so a mid-game
joiner starts with the current sky.

Client apply (per frame, pre-`exeKankyo`): `dKyw_rain_set(intensity)`, set `mSnowCount`,
`mThunderEff.mMode`, `dKy_change_colpat(colpat)` (blended, like the dice proc) — and let kytag00
shelter tags locally modulate as on the host. `wether` (stage-file id) is NOT synced: it is
stage data both sides load identically, and boss-arena weather (kytag06 types 6/7) stays local.

---

## 5. Edge cases & handling notes

| # | Case | Handling |
|---|------|----------|
| 5.1 | **Stage transitions reset time** (stagInfo hour or `nexttime` from the exit) | Host: vanilla behavior is the clock, unchanged. Client: the `setDaytime` replace-hook re-asserts replicated time at the first sim tick of the new stage, clobbering the stage-init write. Client-local `nexttime`/bed-sleep writes are likewise overridden (per design, sleeping changes time only on the host). |
| 5.2 | **Save/load** | `mTime`/`mDate` live in the save. On load, stage init overwrites time (5.1); the client override wins every frame. Host keeps its own save semantics. No extra handling. |
| 5.3 | **Time frozen in events/dungeons** | Host publishes `rate=0` while `event_runCheck()`, message box, `GetTimePass()==0`, `using_time_control_tag`, or `field_0x130a` holds — clients freeze. Because cutscenes are per-triggerer, a client's *own* event does not freeze the world clock (it only gates the vanilla advance, which clients don't run). |
| 5.4 | **Wolf-howl fast-forward** (`time_change_rate=1.0`, slows at 90/285) | Host publishes `rate=2` (plus a TimeEvent at the slowdown boundary, since the vanilla `#if TARGET_PC` slowdown is host-internal). Clients fast-forward until DAWN/DUSK event, then `rate=1`. |
| 5.5 | **Twilight (darkworld)** | Host `setDaytime` runs the `dark_daytime` branch (twilight clock is local story; `dKy_darkworld_check()` = `g_dComIfG_gameInfo.mWorldDark`). Sync still publishes the *daytime* value (host's is forced to 0 in twilight, which is correct — twilight lighting is fixed). Clients: when *their local* darkworld flag is set, keep vanilla darkworld semantics (advance local `dark_daytime`, don't force daytime); when not, force synced time. The `old_time` twilight restore in `dKy_Create` is clobbered on clients by the per-frame override — acceptable (host time wins). |
| 5.6 | **Weather reset per stage** | `dKyw_wether_init()` zeroes everything on stage load, both sides. Host re-sends `WeatherChange` after each stage change; clients re-assert the synced state before the first `dKyw_wether_move()` of the new stage. Include weather in `JoinAccept`/`WorldInit`. |
| 5.7 | **Dice-weather stages** (F_SP108/127/121) | Client suppresses the local dice machine (hook `daKytag06_Draw`, skip `mType==4`); weather comes from sync. Host runs it. kytag06 types 6/7 (boss arenas) keep running locally on both sides (same stage data). |
| 5.8 | **Daybreak wolf-revert** | Vanilla has **no** auto wolf→human at dawn (verified: no `dKy_daynight_check` in `d_a_alink*`, `d_a_midna`; only forced-wolf flags in twilight). The mechanic is coop-planned: each player's `forms` subsystem watches the shared clock (or the reliable DAWN `TimeEvent`) and applies the revert per player. Nothing to sync beyond the time itself. |
| 5.9 | **Night-only spawns / NPC schedules** | All branch on `dKy_daynight_check()` / `dComIfGs_getTime()` → follow the synced time automatically. Spawned geometry is local; no sync. |
| 5.10 | **Room layers at night** | Layers resolve at stage/room load from `dComIfG_get_timelayer()`. A client that loads a stage with a stale time could pick the wrong layer until next room load. Mitigation: the client seeds the host's time from `WorldInit` at join (M4), so when it later TRAVELS to a shared stage normally (M4.5: join-warp removed — no net-layer stage change exists) the engine's time-derived layer resolution matches the host's. |
| 5.11 | **Fishing pond 2x time** (F_SP127/R_SP127) | Covered by `rate=3`; the client clock replicates the **exact vanilla segment rule** — `RatePerTick` returns 0.036/tick (triple) inside 300-60, 0.024/tick (double) inside 150-195, and the vanilla 1× (0.012) fallback elsewhere (M3.5 deepseek M3: the old uniform-2× approximation is gone). Selftest-table-tested against the boundaries. |
| 5.12 | **DEBUG/save-menu time skips** (`time_change_rate=1000+`) | Host-only debug/UI; published via a rate transition + absolute phase in TimeSync. No special handling. |
| 5.13 | **mDate wrap** | `u16` — no realistic wrap (65535 days ≈ 180 years). Ignore. |
| 5.14 | **Day counter drift vs phase wrap** | Both come from the host in TimeSync (absolute) — no accumulation. NEW_DAY fires when the replicated day increments; client runs its local DayProc then. |
| 5.15 | **Weather during events** | `dKy_instant_rainchg` / staff-LIGHT on the host changes the clock/sky → published. On a client, local events' weather writes are overridden by the per-frame sync (host-owned sky). |
| 5.16 | **Host migration** | Out of scope v1 (`00-network.md` §12: session ends when the host leaves). |

---

## 6. Implementation plan

1. **Symbols & headers.** Mod includes `d/d_kankyo.h`, `d/d_kankyo_wether.h`, `d/d_com_inf_game.h`,
   `d/d_save.h`; declares hooks for `dScnKy_env_light_c::setDaytime` (replace),
   `dScnKy_env_light_c::exeKankyo` (pre), `dKy_Create` (post, stage-change re-assert),
   `daKytag06_Draw` (client-only skip of type 4). No stock-game changes — pure `.dusk` mod.

2. **Host side (publisher).**
   - Post-`exeKankyo`: 1 Hz `TimeSync` (`time = dComIfGs_getTime()` or `g_env_light.daytime` —
     equal after `setDaytime`; `day`, `rate` from `time_change_rate`/freeze checks). Detect and
     send `TimeEvent` on day wrap, and on crossing 90/285.
   - Weather: on mode change (and after stage change): `WeatherChange` from `dice_wether_mode`,
     `raincnt`, `mSnowCount`, `mThunderEff.mMode`, `mColpatWeather`.
   - Include time+weather in `JoinAccept`/`WorldInit` payloads.

3. **Client side (replica).**
   - Clock: replace `setDaytime` — advance replicated phase by `rate * simTicks`, handle day wrap
     (local `DayProc`), keep `mDoAud_setHour/Minute/Weekday`, write save via
     `dComIfGs_setTime/setDate`, reset `using_time_control_tag`. Darkworld branch per 5.5.
   - Weather: pre-`exeKankyo` force (`dKyw_rain_set(intensity)`, `mSnowCount`,
     `mThunderEff.mMode`, `dKy_change_colpat(colpat)`); local ramp between weather changes reuses
     the vanilla per-mode target rules. Suppress `daKytag06_Draw` type 4.
   - Stage change: post-`dKy_Create` re-assert time (5.10) and weather before first `wether_move`.

4. **Forms integration (deferred).** Watch DAWN/DUSK `TimeEvent` (or the replicated clock) in the
   per-player `forms` subsystem for the daybreak wolf-revert; no changes to the sync protocol.

5. **Test matrix.** Overworld passage (Hyrule Field dice weather + night spawns), event freeze
   (cutscene starts/stops), kytag11 time-tag stage, Fishing Pond (2x), wolf-howl skip,
   bed-sleep stage change, twilight entry/exit, Snowpeak (snow), boss arenas (D_MN07A),
   mid-game join (WorldInit time/weather), stage change during rain (weather re-assert).

6. **Non-goals.** No weather forecasting/intensity streaming, no host migration, no save-time
   reconciliation (host clock is authoritative; client saves get the synced value).
