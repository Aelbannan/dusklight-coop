# Co-op Review Fixes — Implementation Plan

From the fork review. Scope: priorities 1–6 (concrete bugs/hacks). The long-term
"collapse the story-authority fork" refactor is explicitly OUT of scope.

## Workstreams (parallel, disjoint file sets)

### WS1 — Cleanup + combat attribution fixes
Files: `src/d/actor/d_a_alink.cpp`, `src/d/d_cc_uty.cpp`, `src/d/d_camera.cpp`,
`src/dusk/coop/coop_enemy.cpp`, `include/dusk/coop/coop_types.h`,
`src/dusk/coop/coop_combat.cpp`, `include/dusk/coop/coop_combat.h`

- Remove per-frame `OSReport` debug spam:
  - d_a_alink.cpp `setSingleAnime` (~7336), `allAnimePlay` (~7374), input routing (~9560-9600)
  - d_camera.cpp `onTypeChange` DuskLog frameInterp block (~2121)
- Remove duplicated `setPlayerActor(coopOwner, this)` in `daAlink_c::create()` (keep one).
- Remove no-op loop in `coop_enemy.cpp::processNotedSources`.
- Remove unused `MIN_REQUIRED_LOCAL_VIEWS` from coop_types.h.
- Remove dead `combat::applyPlayerDamage` (cpp + header decl).
- Fix `cc_at_check` (d_cc_uty.cpp:383): don't use `dComIfGp_getPlayer(0)` for
  `getSwordAtUpTime()`; use the attacking actor when it is a daAlink/daPy, else skip the check.
- Fix `~daAlink_c`: clear player status for `alink::ownerOf(this)` instead of hardcoded 0
  (guard TARGET_PC).

### WS2 — Per-player item counters + bottle completion
Files: `include/d/d_com_inf_game.h`, `src/d/d_item.cpp`,
`src/dusk/coop/coop_inventory.cpp`, `include/dusk/coop/coop_inventory.h`,
`src/dusk/coop/coop_bottles.cpp`, `include/dusk/coop/coop_bottles.h`,
`src/d/d_meter2.cpp`

- Item counters: when `dusk_coop_resourcesReady()`, route
  `dComIfGp_setItemRupeeCount` / `dComIfGp_setItemLifeCount` (and clear/get variants as
  needed) directly to `currentPlayer()`'s indexed resources instead of the shared global
  `mItemInfo` counters. Guard the d_meter2.cpp flush blocks (~751-767, ~1202-1211) so they
  no-op under coop (counters stay 0). Solo still goes through the same coop path — one path
  for 1-8 players.
- Bottle completion: hook per-player (via existing bottle bridge pattern):
  `dComIfGs_checkBottle`, `dComIfGs_checkEmptyBottle`, `dComIfGs_checkInsectBottle`,
  `dComIfGs_setBottleItemIn`, `dComIfGs_setEmptyBottleItemIn`, `dComIfGs_setEmptyBottle()`,
  `dComIfGs_setEmptyBottle(u8)`, `dComIfGs_setEquipBottleItemIn`,
  `dComIfGs_setEquipBottleItemEmpty`. This fixes the global fairy check in `checkDeadHP`.
- d_item.cpp: route `item_func_RED_BOTTLE` / `GREEN_BOTTLE` / `OIL_BOTTLE` / etc. through
  `bottles::grantBottleUnlock(currentPlayer(), contents)` like EMPTY_BOTTLE/HALF_MILK.

### WS3 — Render/camera resource lifecycle
Files: `src/dusk/coop/coop_render.cpp`, `include/dusk/coop/coop_render.h`,
`src/dusk/coop/coop_camera.cpp`

- Add `render::releaseCaptureSlot(ViewId)` (uses existing `freeCaptureSlot`) and call it from
  `camera::destroyCamera`; release all slots in `render::reset` / co-op disable.
  NOTE: JKR-heap allocations can't use std::free — track allocation source per slot
  (heapAllocated flag exists; JKR heap memory is reclaimed with the heap — document and only
  std::free the aligned_alloc fallback, or allocate all slots via the same allocator).
- Free `fopCamM_prm_class` params when `fopCamM_Create` fails in `requestSecondaryCreate`.
- Reset `sLoggedWait`/`sLoggedMultiView` (coop_player.cpp) and `sLoggedDualReady`
  (coop_camera.cpp) on reset.

### WS4 — Companion save wiring
Files: `src/dusk/coop/coop_save.cpp`, `include/dusk/coop/coop_save.h`,
`src/d/d_save.cpp` (and memory-card save/load site if needed)

- Find the vanilla save-write completion point and the save-load completion point; call
  `save::saveCompanion(path)` / `save::loadCompanion(path)` there with a companion path
  derived from the main save slot (e.g. `<save>.coop`).
- Ensure ordering: `syncAllFromSave()` seeds all players from the vanilla save first,
  `loadCompanion` then overrides joined players with their companion data.
- Keep solo behavior identical (companion file absent → recoverMissingCompanion path).

## Verification
- Build: `cmake --build build/macos-default-relwithdebinfo` (run once after all WS merge —
  agents must NOT build concurrently).
- Manual smoke: solo boot, 2p join, bottle pickup, rupee pickup by P2, game over flow.
