# M3 adversarial review — time of day & weather sync (deepseek-v4-flash-0731)

Reviewed range: `149b4bc896..HEAD` (commits `c502743698`, `d14aeafb96`, `3a11d7ece0`),
branch `net-coop`. Independent review; cross-checked against the sibling report
(`review-m3-glm-5.2.md`).

## Evidence gathered (all real)

- **Full forced rebuild** of the game: `ninja -t clean dusklight` (2247 files) → clean
  compile+link of every M3-touched TU (`d_kankyo.cpp`, `d_a_kytag06.cpp`, `coop_time.cpp`,
  `coop.cpp`, `session.cpp`, `protocol.cpp`, `selftest_main.cpp`), 33903 exports, only
  pre-existing warnings.
- **Selftest forced rebuild** (`ninja -t clean dusk_net_selftest` → rebuild → run):
  `PASS: all checks succeeded`, including all M3 checks (round-trips, wire sizes, channel
  mapping, JoinAccept/WorldInit carry, host→all broadcasts, rogue-client consumption
  without relay).
- **Net-off boot**: `Dusklight` with defaults (`net.enabled=false`) boots to gameplay
  (kankyo process `fpcNm_KANKYO_e` created, real Link registered), zero errors/warnings,
  no coop-session activity — M3 hooks provably inert.
- **Net-on host+client**: ~2 min live session on 127.0.0.1; client joined (v5), puppets
  spawned, 60 Hz player streaming on both sides, **all M3 client hooks live**
  (setDaytime replica, pre-exeKankyo weather force, dice suppression) — zero errors,
  no asserts, both processes stable.

---

## VERDICT

**M3 meets its acceptance criteria** — same sky on both (absolute phase + derived weather
target adopted, self-correcting), rain arrives together (re-pin on receipt + convergent
ramp at the host's dice rates), cutscene freeze = rate 0 on both (host `ClockFrozen` mirrors
the vanilla gate; client holds at rate 0), stage transitions re-assert (immediate
TimeSync/WeatherChange + `onStageCreate`), single-player untouched (all hooks gated on a
live client session inside `#if TARGET_PC`; verified by the net-off boot). No BLOCKERs.

**One MAJOR protocol-contract deviation** (TimeSync cadence is ~60 Hz, not the specified
1 Hz — functionally benign on LAN but invalidates the documented "1 Hz absolute + rate
replication" model and dead-ens the rate-advance machinery), plus 8 MINORs, two of which
(weather-thunder derivation and the twilight tag gate) are real, if narrow, desync states.

---

## RANKED FINDINGS

### MAJOR 1 — `TimeSync` is published every frame (~60 Hz), not the specified 1 Hz

`src/dusk/coop/coop_time.cpp:65` declares `net::NetClock g_syncClock; // 1 Hz TimeSync cadence`,
but `NetClock` (`include/dusk/net/clock.h:15-17`) is a **60 Hz** cadence helper
(`kRateHz = 60`, `kIntervalUs = 16667`). `coop_time.cpp:268` (`oneSecondDue = g_syncClock.Tick(NowUs())`)
returns true every 16.7 ms and `coop_time.cpp:311` sends whenever `oneSecondDue || stageChanged ||
rate != g_lastRate`. `PublishHostState` runs every game frame (coop `onGameFrame` → `f_ap_game.cpp:826`,
pre-actor-phase), so **TimeSync goes out at frame rate (≈60 Hz), 60× the documented cadence.**

- Contract violated: `docs/design/mod-coop/00-network.md §6` ("TimeSync at 1 Hz"), `04-time-weather.md §4.1`
  ("1 Hz, absolute, self-correcting"), `protocol.h:395` comment ("Unreliable-sequenced, 1 Hz").
- Impact: 8 B × 60 Hz = ~480 B/s/client — bandwidth trivial; the real cost is design-level:
  the rate-replication path (`AdvanceStep`, frozen-hold, pond segments) is nearly **dead code**
  in the common path, because every frame's receipt re-adopts the absolute phase; the
  "immediate on rate change" send (intended to stop clients fast-forwarding within 1 s) is
  redundant; and any cadence-dependent drift (e.g., MINOR 3's pond fallback) is hidden by
  per-frame snapping instead of exercising the documented 1 Hz self-correction.
- Fix: tick the 1 Hz gate explicitly (e.g., `if (g_frameCount % 60 == 0)` or a wall-clock
  1 s interval), keep the immediate sends on stage/rate change, and then re-verify the
  rate-advance path actually runs (see MINOR 7 — nothing tests it today).

### MINOR 1 — Thunder dropped by `DeriveWeather`'s no-rain branch; `ThunderPerMode` then clears synced thunder on non-dice stages

`coop_time.cpp:218` — with `raincnt == 0` the derived mode is `Cloudy/Clear` and **thunder is
not considered** (`outMode = outColpat >= 1 ? Cloudy : Clear; outIntensity = 0`). The stage-file
weather path `daKytag06_wether_proc` case 5 (`d_a_kytag06.cpp:508-517`: `mThunderEff.mMode = 1`,
colpat 1, `dice_rain_minus()` drain) can hold thunder with rain drained to 0. The host then
derives `Cloudy` → no `WeatherChange` on the thunder transition; the client's per-frame
`ThunderPerMode` (`coop_time.cpp:465-471`: Cloudy → `mMode = 0`; Clear → clears a 1) **undoes the
synced thunder `PinWeather` set on receipt**. The host keeps flashing lightning
(`dKyr_thunder_move` spawns `KY_THUNDER` gated on `mThunderEff.mMode < 10`, rain-independent,
`d_kankyo_rain.cpp:5305`) while the client shows none, until the next mode/stage change.
- Trigger is narrow (wether-5 boss arenas with drained rain) — not an acceptance blocker,
  but a real state where the wire's `thunder` field is carried yet actively fought client-side.
- Fix (pick one): (a) in `ThunderPerMode` clear only when `g_weather.thunder == 0` (defer to the
  wire); (b) derive `ThunderLight` for thunder-with-no-rain instead of collapsing to Cloudy;
  (c) gate `ThunderPerMode` to dice stages.

### MINOR 2 — Client darkworld replica omits vanilla's `using_time_control_tag` gate

Vanilla wraps **both** the day advance and the darkworld branch in `if (using_time_control_tag == 0)`
(`d_kankyo.cpp:1547-1550,1606`). The replica's darkworld branch (`coop_time.cpp:533-543`) advances
`dark_daytime` regardless of the tag. With an active kytag11 time-control tag in a twilight stage,
the client's own twilight clock drifts while vanilla (and the host's equivalent) holds.
- Fix: `if (env.using_time_control_tag == 0)` around the darkworld advance — mirror vanilla.

### MINOR 3 — Pond `AdvanceStep` fallback (0.024 = 2×) does not match vanilla 1× outside the two special windows

Vanilla pond block (`d_kankyo.cpp:1575-1584`): base advance 1×, +2× inside 300-60, +1× inside
150-195 → **1× everywhere else**. `AdvanceStep` (`coop_time.cpp:366-378`) implements the two
windows exactly (0.036 / 0.024) but falls back to `0.024` (2×) instead of `0.012` — the client
over-advances 0.012/tick for 58% of the pond day. 04 §5.11 blessed the "uniform 2x"
approximation, but since the code already implements the exact window rule, the fallback should
be `0.012`; currently the drift is only bounded by the (broken-1 Hz) absolute adoption — invisible
at the 60 Hz cadence, up to ~0.36° snap-back per second if MAJOR 1 is fixed.

### MINOR 4 — `onStageCreate` "before room-layer resolution" comment is inaccurate

`dStage_Create` resolves the start-room layer **before** the kankyo process exists:
`d_stage.cpp:2758` `dStage_roomInit(...)` → layer tables chosen via
`dComIfG_play_c::getLayerNo(0)` (`d_stage.cpp:2593,2604`) → `dKy_daynight_check()` →
`dComIfGs_getTime()`; only then `d_stage.cpp:2763` `dKankyo_create()` → `dKy_Create` →
`onStageCreate` (`d_kankyo.cpp:8381`). So `coop_time.cpp:589`'s claim ("re-assert ... before
room-layer resolution") is wrong — the start-room layer is chosen with the pre-assert time.
Actual behavior still matches 04 §5.10's accepted worst case: the pre-assert time is the previous
stage's end ≈ the synced time (drift bounded), so a wrong layer occurs only if dusk/dawn crosses
during the load, corrected at the next room change. Fix the comment; a true pre-roomInit assert
would need a `dStage_Create` hook.

### MINOR 5 — `kTimeFlagDarkworld` is published/seeded/compared but never consumed

Flags are derived (`coop_time.cpp:266`), carried in TimeSync (`:269-274`), re-seed-compared
(`:690`), but the client replica keys **only** on its own `dKy_darkworld_check()`
(`coop_time.cpp:530`) — per 04 §5.5 that is the design, so the wire bit is dead weight today.
Consequence (accepted in 5.5, undocumented in code): a non-twilight client whose host is in a
twilight stage forces synced daytime = 0 → permanent midnight look in its own stage. Note it or
consume the bit.

### MINOR 6 — Dice `UNK6` drain-rate mismatch

Host `DICE_MODE_UNK6` drains raincnt 2/frame (`d_a_kytag06.cpp:232-239`); the derived
Clear-mode client drain (`DiceRainMinus`, `coop_time.cpp:396-404`) drains ~0.75/frame
aggregate. Narrow (requires `field_0x130b == 1`), bounded by the next mode change — cosmetic.

### MINOR 7 — Selftest covers wire/routing only; game-side publish/replica logic has zero automated coverage

`dusk_net_selftest` links only net-layer TUs (`CMakeLists.txt:736-751`) — `coop_time.cpp` is not
in it, which is exactly how MAJOR 1 (cadence) and MINORs 1-3 shipped untested. The M3 selftest
(`selftest_main.cpp:1051-1252`) is good on wire round-trips/sizes/channels/broadcast/rogue-policy.
Add: a cadence check (count TimeSyncs over a window), and a table-driven `DeriveWeather` test.

### MINOR 8 — `00-network.md §5` time/weather rows are stale vs the v5 wire

§5 table rows (`00-network.md:117-119`): `TimeSync | ... | 1 Hz | time phase (u32) + elapsed delta`,
`TimeEvent | event id + time`, `WeatherChange | weather id + intensity` — these are the M0
placeholders; `protocol.h:392-426` (v5) carries `f32 time 0..360 + day + rate + flags` /
`eventId + pad + time + day` / `mode + thunder + intensity + colpat`. The §4/§5 "WorldInit ...
no state sections" wording also predates v5. §8 text is generic and consistent. Update the table.

---

## Edge cases (04 §5.1-§5.16) — handled vs deferred

| # | Case | Status | Evidence |
|---|------|--------|----------|
| 5.1 | Stage-transition time reset | **Handled** | Host: immediate TimeSync+WeatherChange on stage-name change (`coop_time.cpp:274-285,311,319-324`); client: `onStageCreate` re-assert (`d_kankyo.cpp:8381`) |
| 5.2 | Save/load | **Handled** | Absolute phase+day adoption on every sync; client save carries synced values (`coop_time.cpp:501-510`) |
| 5.3 | Event freeze | **Handled** | `ClockFrozen` mirrors the vanilla gate exactly (`coop_time.cpp:98-113` vs `d_kankyo.cpp:1547-1558`); client holds at rate 0. (Masked by MAJOR 1 cadence, correct in design; client's own events don't freeze the world clock — intended) |
| 5.4 | Wolf-howl fast-forward | **Handled** | `rate=2` published (`coop_time.cpp:127-130`); fork's own `#if TARGET_PC` slowdown runs natively on the host (`d_a_alink_wolf.inc:4181`, `d_kankyo.cpp:1566-1571`) |
| 5.5 | Twilight/darkworld | **Handled** | Client-own-flag branch keeps vanilla dark clock (`coop_time.cpp:530-543`); host pins daytime 0 + frozen rate. Caveats: MINOR 2 (tag gate), MINOR 5 (flags bit unused) |
| 5.6 | Weather reset per stage | **Handled** | Host re-sends on stage change; client `onStageCreate` pin + per-frame force survives `dKyeff_Create`'s `dKyw_wether_init` zeroing (KANKYO created before KYEFF, `d_stage.cpp:2508-2509`; force runs pre-`exeKankyo`) |
| 5.7 | Dice stages | **Handled** | Client suppresses type-4 (`d_a_kytag06.cpp:301`, R16 verified — the branch drives weather state only); boss-arena (6/7) weather deliberately carried on the same channel (documented deviation from §5.7 "stays local") — caveat MINOR 1 |
| 5.8 | Daybreak wolf-revert | **Deferred** | DAWN/DUSK `TimeEvent` stored for the future `forms` subsystem; nothing applied in M3 — correct per plan |
| 5.9 | Night-only spawns / NPC schedules | **Handled** | Branch on the synced save time — free |
| 5.10 | Room layers at night | **Handled (worst case accepted)** | Caveat MINOR 4: the re-assert runs after start-room layer resolution; wrong layer only if dusk/dawn crosses mid-load, corrected next room change — per design |
| 5.11 | Fishing Pond 2× | **Handled (approx.)** | Exact window rates + 2× fallback (caveat MINOR 3) |
| 5.12 | DEBUG/menu time skips | **Handled** | `rate>=1000 → frozen` + immediate absolute sync (`coop_time.cpp:133-137`) |
| 5.13 | mDate wrap | Ignored per design (u16, ~180 yr) | — |
| 5.14 | Day counter vs phase wrap | **Handled** | Absolute day in TimeSync; NEW_DAY advisory clears temp bit 91 (idempotent with the replica wrap) |
| 5.15 | Weather during events | **Handled** | Host-owned sky; client local event writes overridden per frame |
| 5.16 | Host migration | Out of scope v1 | — |

No case **silently mis-syncs** beyond the two documented narrow states (MINOR 1 thunder,
MINOR 2 twilight tag) and the accepted approximations (MINOR 3 pond).

---

## VERIFIED-OK

- **Build**: full clean rebuild of the game; all M3 TUs compile+link; M1/M2 paths unaffected
  (protocol struct growth is additive; `WeatherId`/`TimeInfo` removal has no other users).
- **Selftest**: passes on a forced clean rebuild, including every M3 assertion.
- **Boot, net off**: game runs to gameplay with the kankyo process normal and zero M3 activity —
  byte-identical vanilla time/weather path structurally guaranteed (hooks gated on
  `sessionActive() && !hostRole()`; `EnsureSession` stops when `net.enabled=false`).
- **Live host+client**: 2 min session, M3 hooks live on the client, zero errors.
- **Guard hygiene**: all game-side hooks are inside `#if TARGET_PC` with the non-PC path
  textually identical to `149b4bc896` (`d_kankyo.cpp:1540/8240/8381`, `d_a_kytag06.cpp:301`);
  the coop_time include is inside `#if TARGET_PC` (`d_kankyo.cpp:39`).
- **Wire & routing**: layouts match `protocol.h`; serializers symmetric
  (`protocol.cpp:47-66,259-276,354-372`); `WireSize` updated; `kProtocolVersion` 4→5
  (`protocol.h:61`); channels unreliable(reliable) as specified; `PolicyFor` = None for
  time/weather (`session.cpp:61-70`), rogue client copies consumed, never relayed
  (selftest-verified); host broadcasts via `SendGameMessage`→`SendToAll` (`session.cpp:511-522`).
- **JoinAccept/WorldInit** carry the host's clock+sky (`session.cpp:392-417`); host updates the
  session world state every frame (`coop_time.cpp:318-326`); client re-seed guarded by the
  `g_worldSeen*` comparison (`coop_time.cpp:684-697`). I checked the sibling review's "re-seed
  can regress a fresher TimeSync" claim and found it **not reproducible**: the host's
  `worldTime_` is updated in the same publish that sends TimeSync, so a `WorldInit`
  re-broadcast always carries exactly the last TimeSync's values — a re-seed is value-identical.
- **Rate model**: every `time_change_rate` setter in the tree is 0.012/0.0/1.0/1000+ — the
  bucket model covers all real rates; `ClockFrozen` mirrors the vanilla gate including the
  message-box `mode >= 2` check and `field_0x130a`.
- **Dice suppression (R16)**: `daKytag06_type_04_Execute` → `dKy_event_proc` touches weather
  state only (`d_a_kytag06.cpp:261-262,66-258`); the draw still returns 1; all other kytag06
  types (1,2,3,5,6,7,8,9,10,11) still run; kytag00 shelter tags still run locally — the rain
  ramp converges (not re-pins) so shelter caps/restores survive to the next `wether_move`; the
  colpat pin defers to the actor-phase gather state (verified against `dKy_change_colpat`'s
  gather-state semantics, `d_kankyo.cpp:9501` area, and `exeKankyo`'s gather logic `:4733-4786`).
- **No M4 creep**: range touches only time/weather + session/protocol/selftest; no join-warp
  (JoinAccept stage stays empty; no `dStage_changeScene`), no forms/wolf-revert; `shutdown()`
  resets all statics; no dead code left in the module.

---

## MUST-FIX before M4

1. **MAJOR 1** — make `TimeSync` actually 1 Hz (the `NetClock` is 60 Hz; `oneSecondDue` is
   misnamed). Add a selftest cadence check so it cannot regress.
2. **MINOR 1** — decide the thunder policy for non-dice stages and make `ThunderPerMode`
   defer to the wire (`g_weather.thunder`) instead of clearing synced thunder.
3. **MINOR 2** — gate the replica's darkworld advance on `using_time_control_tag == 0`
   (vanilla parity).

MINORs 3-8 are non-blocking but cheap: fix the pond fallback to `0.012`, correct the
`onStageCreate` ordering comment (MINOR 4), consume or annotate the darkworld flag bit,
update `00-network.md §5` to v5, and table-test `DeriveWeather`.
