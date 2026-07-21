# Phase 6 — Secondary `daAlink_c`

**Date:** 2026-07-21  
**Status:** Initial implementation (in-engine verification pending)

## What changed

- Press-Start join spawns `fpcNm_ALINK_e` via pending-owner registry (`dusk::coop::alink::*`)
- Secondary create skips: `setPlayer(0)` / `setLinkPlayer`, restart room, Midna spawn, story wolf force, ride starts
- Secondary uses isolated `secondaryBgWaitFlag` (does not share P0 `bgWaitFlg`)
- `field_0x317c` set to player/view id for cam-relative move
- `setStickData` reads `input::snapshot` for secondary Links
- Destructor clears sidecar only for secondaries; never nulls P0 globals for P1 delete

## How to test

1. Configure/build with `-DENABLE_LOCAL_COOP=ON`
2. Run game (no `-D` on the executable)
3. Load a field stage
4. Second pad **Start** → look for:
   - `[coop] Player N joined via Start …`
   - `[coop] Secondary Link PN spawn requested …`
   - `[coop] Secondary Link PN created …` / `ready`
5. Confirm `dComIfGp_getPlayer(0)` / Link still P0
6. Move second stick — P1 should walk/roll/jump with real Alink anims
7. Change rooms — P1 should recreate

## Known gaps

- Many systems still call `daAlink_getAlinkActorClass()` / `getPlayer(0)` (enemies/events target P0)
- Direct `PAD_1` reads outside `setStickData` still ignore P1
- Wolf Midna / horse ride for P1 still limited
- Proxy actor profile remains compiled but unused for join
