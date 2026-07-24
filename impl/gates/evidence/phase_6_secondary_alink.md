# Phase 6 — Secondary `daAlink_c`

**Date:** 2026-07-22  
**Status:** Real Links only; up to 8 local players

## What changed

- Press-Start join spawns `fpcNm_ALINK_e` (real `daAlink_c`) — no custom coop/proxy actor
- Per-player pending create state (FIFO) so P1–P7 can create concurrently
- Secondary create skips: `setPlayer(0)` / `setLinkPlayer`, restart room, Midna spawn, story wolf force, ride starts
- Secondary uses isolated per-player `bgWaitFlag` (does not share P0 `bgWaitFlg`)
- `field_0x317c` set to player/view id for cam-relative move
- `setStickData` reads `input::snapshot` for secondary Links
- Destructor clears sidecar only for secondaries; never nulls P0 globals for P1+ delete
- Multi-view tiling for 3–8 players; dual composite remains the 2P path
- Eight spawn offsets keyed by joining player id

## How to test

1. Configure/build with a normal PC build
2. Run game (no `-D` on the executable)
3. Load a field stage
4. Press **Start** on pads 2…8 → look for:
   - `[coop] Player N joined via Start …`
   - `[coop] Secondary Link PN spawn requested …`
   - `[coop] Secondary Link PN created …` / `ready`
5. Confirm `dComIfGp_getPlayer(0)` / Link still P0
6. Each joined pad should drive its own Link
7. Change rooms — secondaries should recreate

## Known gaps

- Many systems still call `daAlink_getAlinkActorClass()` / `getPlayer(0)` (enemies/events target P0)
- Direct `PAD_1` reads outside `setStickData` still ignore secondary players
- Wolf Midna / horse ride for P1+ still limited
- 8-view tiled performance not yet profiled
