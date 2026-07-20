# Gate E — Resource Adapter

**Status:** 🟡 PoC implemented (in-engine verification pending)

**Depends on:** Nothing

## Deliverables

- [x] Player 0 reads/writes original save fields
- [x] Player 1 uses sidecar resource storage
- [x] Global bow with separate arrow counts per player
- [x] Global bottle slot unlock with separate contents per player
- [x] Original save loads without companion file
- [x] Mismatched or missing companion recovers safely

## Acceptance criteria

- Player 0 resource changes are reflected in original save
- Player 1 cannot consume Player 0's arrows or rupees
- Both players can equip the same global item simultaneously
- Bottle unlock is global; initial contents go to collector only
- No companion file → Player 1 initializes from global progression
- Corrupt companion → recovery log + reinitialize affected players

## Implementation notes (2026-07-20)

### Working

| Area | Detail |
|------|--------|
| P0 sync | `inventory::syncPlayer0FromSave` / `syncPlayer0ToSave` map life, magic, oil, rupees, arrows, pachinko, bomb counts, bottle contents/qty against `dSv_player_status_a_c`, `dSv_player_item_record_c`, `dSv_player_item_c`. P0 coop API getters/setters read/write original save directly (sidecar is a cache). |
| P1+ sidecar | `PlayerResources` / `PlayerLoadout` in `Runtime::playerRuntime[]` |
| Capacities | Max life/magic/oil/arrows/rupees/bomb bags remain global (original save) |
| Permanent items | `hasGlobalItem` / `unlockGlobalItem` + `dComIfGs_isItemFirstBit` authority |
| Choke-point hooks | `dComIfGs_get/set{Life,Rupee,Oil,Magic,ArrowNum,PachinkoNum,BombNum,BottleNum}` and bottle-slot `get/setItem(SLOT_11..14)` route to sidecar when `isEnabled() && currentPlayer()!=0` via `coop_resource_bridge.h` |
| Bottles | Unlock mask derived from original `SLOT_11..14`; `onBottleUnlock` / `grantBottleUnlock`; empty bottle item gets hooked in `d_item.cpp` |
| Companion I/O | Binary `.coop` file: magic `COOP`, version, CRC32 payload; players 1+ only; missing/corrupt → `recoverMissingCompanion` / `recoverCorruptCompanion` |

### Stubbed / deferred

| Area | Detail |
|------|--------|
| Full `dSv_player_item_c` methods | `setBottleItemIn` / `checkBottle` still walk the single original `mItems[]` — fill-from-empty via those paths can still touch P0 unless called through `dComIfGs_*` or `bottles::*` |
| Bow grant starter ammo for all joined | `item_func_BOW` still writes P0 arrow count; late joiners get starter via `initSecondaryFromGlobal` |
| Select-item loadout hot path | P0 loadout sync exists; live divert of `getSelectItemIndex` for P1+ not wired yet (equip-same-item OK via global `mItems` + per-player loadout storage) |
| Save-menu integration | Callers must invoke `save::loadCompanion` / `saveCompanion` beside original card save (path convention: `slot-N-save.coop`) |
| In-game verification | Needs `ENABLE_LOCAL_COOP=ON` two-player session |

### Key save field map

| Resource | Original accessor |
|----------|-------------------|
| Life / max life | `dSv_player_status_a_c::{get,set}Life` / `MaxLife` |
| Rupees / wallet max | `::{get,set}Rupee` / `getRupeeMax()` |
| Oil / max oil | `::{get,set}Oil` / `MaxOil` |
| Magic / max magic | `::{get,set}Magic` / `MaxMagic` |
| Arrows / quiver | `itemRecord.getArrowNum` / `itemMax.getArrowNum` |
| Bombs | `itemRecord.getBombNum(bag)` / `itemMax.getBombNum(type)` |
| Bottles | `item.getItem(SLOT_11+i)` + `itemRecord.getBottleNum(i)` |

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| 2026-07-20 | Implemented sync, sidecars, bridge hooks, companion CRC load/save/recover, bottle unlock grant | Code complete; runtime test pending |

## Test plan

1. Build with `-DENABLE_LOCAL_COOP=ON` (PC).
2. Load a vanilla save with **no** `.coop` file → P0 resources match save; joining P1 gets empty bottles for unlocked slots, starter ammo if bow unlocked, not P0's rupee/arrow stacks.
3. P0 spend arrows/rupees → original save fields change after `syncPlayer0ToSave` / natural `dComIfGs_set*`.
4. Under `ScopedContext` for P1, spend arrows/rupees → P0 counts unchanged.
5. Unlock empty bottle as P1 → original save gains empty slot; P1 holds contents; other players get empty bottles.
6. Write companion via `saveCompanion(path)`; reload; corrupt magic/CRC → recovery log + reinit.
7. Both players select bow (global `SLOT_4`) with independent arrow counts.

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
