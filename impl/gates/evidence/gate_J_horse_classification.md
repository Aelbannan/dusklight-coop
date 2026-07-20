# Gate J Evidence — Horse Query Call-Site Classification

Audit of `dComIfGp_getHorseActor` / `setHorseActor` / `dComIfGs_*HorseRestart*` / horse-self callbacks.
Compat routing: when `ENABLE_LOCAL_COOP` and co-op enabled, `dComIfGp_getHorseActor()` → `dusk::coop::horses::resolveForContext()` (mounted if available, else owned for `currentPlayer()`; ambient/`activePlayer==0` → Player 0).

## Classes

| Class | Meaning |
|-------|---------|
| **OWNED_HORSE** | Horse bound to a specific owner player (or the horse actor itself) |
| **MOUNTED_HORSE** | Rider’s horse via `mRideAcKeep` / ride flag; owned≈mounted under correct context |
| **ACTUAL_COLLIDING_ACTOR** | Must use the horse involved in the collision / poly hit |
| **EVENT_HORSE** | Story/event path; Player 0’s event horse is authoritative |
| **PLAYER0_COMPATIBILITY** | Intentionally Player 0 / debug-global singleton |
| **UNSAFE_UNRESOLVED** | Ambiguous — gate fails while any remain |

## Summary (2026-07-20)

| Class | Count (approx) |
|-------|----------------|
| OWNED_HORSE | 12 |
| MOUNTED_HORSE | ~55 |
| ACTUAL_COLLIDING_ACTOR | 12 |
| EVENT_HORSE | 14 |
| PLAYER0_COMPATIBILITY | 18 |
| UNSAFE_UNRESOLVED | **0** |

---

## `dComIfGp_setHorseActor`

| File | Site | Class | Notes |
|------|------|-------|-------|
| `d_a_horse.cpp` create | register after heap | OWNED_HORSE | P0 only when `shouldRegisterGlobally`; secondary uses `onCreateSuccess` / sidecar |
| `d_a_horse.cpp` dtor | clear if global == this | OWNED_HORSE | Uses raw global compare; sidecar cleared via `onHorseDestroyed` |
| `coop_accessors.cpp` `setOwnedHorse(0)` | P0 write-through | OWNED_HORSE | Secondary must not call |

## `dComIfGs_*HorseRestart*`

| File | Site | Class | Notes |
|------|------|-------|-------|
| `d_a_horse.cpp` create | read restart pose | PLAYER0_COMPATIBILITY | Skipped for secondary pending create |
| `d_a_horse.cpp` `savePos` | write restart | OWNED_HORSE | Secondary → `horses::setRestart`; P0 → original save |
| `ImGuiMenuTools.cpp` | debug display | PLAYER0_COMPATIBILITY | Tools UI |

## `d_a_horse.cpp` (self / callbacks)

| Site | Class | Notes |
|------|-------|-------|
| Singleton reject in `create` | OWNED_HORSE | Secondary bypass; P0 uses raw global |
| `daHorse_searchEnemy` | OWNED_HORSE | Fixed: `HorseSearchContext` carries `this` |
| `daHorse_searchSceneChangeArea` | OWNED_HORSE | Fixed: executor passes `this` |
| Line ~490 legacy getHorseActor (removed path) | — | Replaced |

## Link horseback (`d_a_alink*.inc` / `d_a_alink.cpp`)

Sites gated by `checkHorseRide()` / ride init / get-off / demo ride are **MOUNTED_HORSE**.
Under co-op context they resolve via compat owned horse; full per-Link `mRideAcKeep` authority remains the long-term target (`mountedHorseForLink` / `resolveMounted` ready). Proxy players cannot ride yet.

| File | Class | Notes |
|------|-------|-------|
| `d_a_alink_horse.inc` (~33 gets) | MOUNTED_HORSE | Ride/wait/cut/jump/bow/etc. |
| `d_a_alink.cpp` horse-start / demo / exit / shadow | MOUNTED_HORSE | Ride-aware |
| `d_a_alink_whistle.inc` | OWNED_HORSE | Call/summon owned Epona |
| `d_a_alink_bow.inc` | MOUNTED_HORSE | Horseback aim |
| `d_a_alink_damage.inc` | MOUNTED_HORSE | Horseback damage |
| `d_a_alink_demo.inc` | EVENT_HORSE / MOUNTED_HORSE | Demo force-ride vs ride checks |

## Collision / background

| File | Class | Notes |
|------|-------|-------|
| `d_bg_w.cpp` ChkHorse | ACTUAL_COLLIDING_ACTOR | Horse poly → context/P0 horse; multi-horse poly map deferred, safe under P0-primary collision |
| `d_bg_w_kcol.cpp` ChkHorse | ACTUAL_COLLIDING_ACTOR | Same |
| `d_a_obj_rgate.cpp` / `d_a_obj_kgate.cpp` | ACTUAL_COLLIDING_ACTOR | Gate vs horse push; rider-gated |
| `d_a_arrow.cpp` | ACTUAL_COLLIDING_ACTOR / MOUNTED_HORSE | Arrow vs horse |

## Enemies (speed / ride checks)

Typically `checkHorseRide() && getHorseActor()->speedF` → **MOUNTED_HORSE** (player 0’s mounted horse under ambient context; under per-player combat context → that player).

| File | Class |
|------|-------|
| `d_a_e_rd.cpp`, `d_a_e_rdy.cpp`, `d_a_e_mf.cpp`, `d_a_e_dn.cpp`, `d_a_e_yr.cpp`, `d_a_e_kr.cpp`, `d_a_e_fk.cpp`, `d_a_e_wb.cpp`, `d_a_b_gnd.cpp` | MOUNTED_HORSE |
| `d_a_bd.cpp` | MOUNTED_HORSE |

## NPC / tags / events

| File | Class | Notes |
|------|-------|-------|
| `d_a_hozelda.cpp` | EVENT_HORSE | Zelda-on-Epona event actor |
| `d_a_tag_event.cpp` rodeo | EVENT_HORSE | |
| `d_a_tag_hinit.cpp`, `tag_hjump`, `tag_hstop`, `tag_camera` | EVENT_HORSE / OWNED_HORSE | Horse tags; ambient → P0 |
| `d_a_npc_aru/besu/bou/kolin/maro/post/taro.cpp` | EVENT_HORSE / PLAYER0_COMPATIBILITY | NPC horse awareness |
| `d_a_e_warpappear.cpp`, `d_a_no_chg_room.cpp`, `d_a_swc00.cpp` | PLAYER0_COMPATIBILITY | Stage/utility |
| `d_a_npc_*` horse angle searches | PLAYER0_COMPATIBILITY | Story NPC vs P0 Epona |

## Camera / meter / graphics / tools

| File | Class | Notes |
|------|-------|-------|
| `d_camera.cpp`, `d_ev_camera.cpp` | MOUNTED_HORSE / EVENT_HORSE | View follows ride or event horse |
| `d_meter2.cpp` | PLAYER0_COMPATIBILITY | HUD rodeo check |
| `m_Do_graphic.cpp` | PLAYER0_COMPATIBILITY | |
| `ImGuiMenuTools.cpp` | PLAYER0_COMPATIBILITY | |

## Co-op runtime

| File | Class | Notes |
|------|-------|-------|
| `coop_accessors.cpp` | OWNED_HORSE | Sidecar get/set; P0 raw global |
| `coop_horses.cpp` | OWNED_HORSE | Registry / spawn / resolve |

## Gaps / deferred (not UNSAFE)

- Full `d_a_alink_horse.inc` rewrite to `getMountedHorse()` / `mRideAcKeep` — API ready; bulk replace deferred until secondary `daAlink_c`.
- Eight-horse heap/audio telemetry — not captured this PoC.
- Companion-save persistence for secondary restart beyond in-memory `HorseSlot` — stubbed.
- Horse-vs-horse soft collision policy — not implemented.
