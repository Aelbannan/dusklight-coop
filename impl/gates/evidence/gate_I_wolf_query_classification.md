# Gate I Evidence — Wolf / Transform Query Classification

**Date:** 2026-07-20

## Policy

| Kind | Meaning |
|------|---------|
| CURRENT_LINK | Active co-op context player (SP: global Link) via diverted `checkNowWolf()` |
| SPECIFIC_ACTOR | Named player pointer; use `checkWolf()` instance flag |
| VIEW_OWNER | Camera / senses for the view being drawn |
| EVENT_PARTICIPANT | Story/event participant (often P0 until event adapter expands) |
| STORY_AUTHORITY | Standalone Midna + canonical Link (`checkNowWolfAuthority`) |
| GLOBAL_UNLOCK | Original save transform status / progression |
| UNSAFE_UNRESOLVED | Must not remain |

## Summary

| Kind | Count |
|------|------:|
| CURRENT_LINK | 294 |
| SPECIFIC_ACTOR | 0 |
| VIEW_OWNER | 80 |
| EVENT_PARTICIPANT | 11 |
| STORY_AUTHORITY | 9 |
| GLOBAL_UNLOCK | 12 |
| UNSAFE_UNRESOLVED | 0 |
| **Total sites scanned** | **406** |

UNSAFE_UNRESOLVED remaining: **0**


## SPECIFIC_ACTOR divert (checkNowWolf → checkWolf)

These sites held a player pointer but called the **static** `checkNowWolf()` via the instance.
They now use `checkWolf()` so the named actor is queried.

| File | Sites converted |
|------|----------------:|
| `src/d/actor/d_a_b_tn.cpp` | 1 |
| `src/d/actor/d_a_b_mgn.cpp` | 2 |
| `src/d/actor/d_a_tag_camera.cpp` | 1 |
| `src/d/actor/d_a_obj_yousei.cpp` | 1 |
| `src/d/actor/d_a_cow.cpp` | 2 |
| `src/d/actor/d_a_door_shutter.cpp` | 5 |
| `src/d/actor/d_a_e_rdb.cpp` | 1 |
| `src/d/actor/d_a_obj_waterfall.cpp` | 1 |
| `src/d/actor/d_a_tbox.cpp` | 2 |
| `src/d/actor/d_a_obj_digholl.cpp` | 1 |
| `src/d/actor/d_a_obj_chest.cpp` | 1 |
| `src/d/actor/d_a_e_vt.cpp` | 4 |
| `src/d/actor/d_a_e_th.cpp` | 2 |
| `src/d/actor/d_a_tag_wljump.cpp` | 1 |
| **Total** | **25** |

Classification count for SPECIFIC_ACTOR in the live `checkNowWolf` scan is 0 because those call sites no longer mention `checkNowWolf`.
## Central diverts

- `daPy_py_c::checkNowWolf()` → `dusk_coop_checkNowWolf()` (CURRENT_LINK)
- `daPy_py_c::checkNowWolfAuthority()` → P0 / global Link (STORY_AUTHORITY); Midna call sites updated
- `daPy_py_c::checkNowWolfEyeUp()` → view/player senses bridge
- `dComIfGs_setTransformStatus` → `dusk_coop_trySetTransformStatus` (P0 writes save; P1+ sidecar)
- `player->checkNowWolf()` mistaken static-via-instance sites → `player->checkWolf()` (SPECIFIC_ACTOR)

## Classification table

| Kind | File | Line | Reason |
|------|------|-----:|--------|
| CURRENT_LINK | `include/d/actor/d_a_formation_mng.h` | 183 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `include/d/actor/d_a_guard_mng.h` | 27 | Default: diverted static checkNowWolf |
| CURRENT_LINK | `include/d/actor/d_a_player.h` | 1168 | API definition / divert |
| CURRENT_LINK | `include/d/actor/d_a_player.h` | 1170 | Default: diverted static checkNowWolf |
| CURRENT_LINK | `include/d/actor/d_a_player.h` | 1177 | API definition / divert |
| CURRENT_LINK | `include/d/actor/d_a_player.h` | 1179 | API definition / divert |
| CURRENT_LINK | `include/d/actor/d_a_player.h` | 1181 | Default: diverted static checkNowWolf |
| CURRENT_LINK | `include/d/d_com_inf_game.h` | 1445 | Default classification |
| CURRENT_LINK | `include/d/d_save.h` | 164 | Default classification |
| CURRENT_LINK | `src/d/actor/d_a_alink_wolf.inc` | 225 | Link metamorphose (diverted via notifyLinkFormChanged) |
| CURRENT_LINK | `src/d/actor/d_a_alink_wolf.inc` | 427 | Link metamorphose (diverted via notifyLinkFormChanged) |
| CURRENT_LINK | `src/d/actor/d_a_b_ds.cpp` | 1121 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_ds.cpp` | 1292 | Default classification |
| CURRENT_LINK | `src/d/actor/d_a_b_gg.cpp` | 714 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_gg.cpp` | 750 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_gg.cpp` | 758 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_gg.cpp` | 769 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_gg.cpp` | 799 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_gg.cpp` | 807 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_gg.cpp` | 861 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_gg.cpp` | 886 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_gg.cpp` | 908 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_gg.cpp` | 916 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_gg.cpp` | 1301 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_gg.cpp` | 1436 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_gg.cpp` | 3286 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_gg.cpp` | 3299 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_gm.cpp` | 1415 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_gm.cpp` | 1555 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_mgn.cpp` | 1475 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_mgn.cpp` | 2464 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_mgn.cpp` | 2685 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_mgn.cpp` | 2693 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_mgn.cpp` | 2927 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_mgn.cpp` | 2955 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_mgn.cpp` | 3031 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_tn.cpp` | 976 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_tn.cpp` | 1002 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_tn.cpp` | 1027 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_tn.cpp` | 1042 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_tn.cpp` | 1708 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_tn.cpp` | 1729 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_tn.cpp` | 1766 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_tn.cpp` | 1773 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_tn.cpp` | 3671 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_tn.cpp` | 3708 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_tn.cpp` | 3885 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_tn.cpp` | 4049 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_b_tn.cpp` | 4309 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_cow.cpp` | 807 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_cow.cpp` | 816 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_cow.cpp` | 1209 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_cow.cpp` | 1296 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_cow.cpp` | 1552 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_cow.cpp` | 1567 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_do.cpp` | 1906 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_do.cpp` | 2198 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_door_boss.cpp` | 273 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_door_bossL1.cpp` | 808 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_door_bossL1.cpp` | 819 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_door_bossL5.cpp` | 371 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_door_knob00.cpp` | 269 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_door_mbossL1.cpp` | 1296 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_door_mbossL1.cpp` | 1311 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_door_mbossL1.cpp` | 1345 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_door_shutter.cpp` | 207 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_door_shutter.cpp` | 238 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_door_shutter.cpp` | 261 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_door_shutter.cpp` | 396 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_door_shutter.cpp` | 435 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_door_shutter.cpp` | 489 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_door_shutter.cpp` | 517 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_door_shutter.cpp` | 1826 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_door_shutter.cpp` | 1888 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_bug.cpp` | 390 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_bug.cpp` | 408 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_bug.cpp` | 645 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_dn.cpp` | 1781 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_dn.cpp` | 1922 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_dn.cpp` | 1943 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_fs.cpp` | 261 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_gi.cpp` | 288 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_gi.cpp` | 926 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_hp.cpp` | 316 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_mf.cpp` | 1675 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_mf.cpp` | 1807 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_mf.cpp` | 1828 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_po.cpp` | 203 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_po.cpp` | 624 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_po.cpp` | 937 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_pz.cpp` | 594 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_pz.cpp` | 675 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_pz.cpp` | 741 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_pz.cpp` | 774 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_pz.cpp` | 811 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_pz.cpp` | 1019 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_pz.cpp` | 1034 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_pz.cpp` | 1054 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_pz.cpp` | 1066 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_pz.cpp` | 1131 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_rd.cpp` | 2843 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_rd.cpp` | 2980 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_rd.cpp` | 3006 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_rd.cpp` | 6005 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_rd.cpp` | 6675 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_rdy.cpp` | 2126 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_rdy.cpp` | 2228 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_rdy.cpp` | 2249 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_rdy.cpp` | 4553 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_s1.cpp` | 384 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_s1.cpp` | 403 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_s1.cpp` | 427 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_s1.cpp` | 982 | Default classification |
| CURRENT_LINK | `src/d/actor/d_a_e_vt.cpp` | 1455 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_ww.cpp` | 1223 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_ww.cpp` | 1243 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_e_ww.cpp` | 1766 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_formation_mng.cpp` | 97 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_fr.cpp` | 415 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_fr.cpp` | 447 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_fr.cpp` | 515 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_fr.cpp` | 589 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_horse.cpp` | 1559 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_horse.cpp` | 2380 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_horse.cpp` | 3826 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_kytag03.cpp` | 370 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_kytag12.cpp` | 310 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_kytag12.cpp` | 391 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_kytag12.cpp` | 414 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_kytag12.cpp` | 617 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_kytag12.cpp` | 693 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_kytag12.cpp` | 908 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_myna.cpp` | 512 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_myna.cpp` | 563 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_myna.cpp` | 610 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_myna.cpp` | 690 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_myna.cpp` | 1013 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_myna.cpp` | 1084 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_myna.cpp` | 1255 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_myna.cpp` | 1277 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_ni.cpp` | 1493 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_ni.cpp` | 1731 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc.cpp` | 2350 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc.cpp` | 2362 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc.cpp` | 2408 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc2.cpp` | 526 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_aru.cpp` | 577 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_aru.cpp` | 589 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_aru.cpp` | 810 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_aru.cpp` | 1872 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_ashB.cpp` | 746 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_ashB.cpp` | 1092 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_bans.cpp` | 725 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_bans.cpp` | 1700 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_cd2.cpp` | 922 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_cdn3.cpp` | 1386 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_chat.cpp` | 3877 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_chin.cpp` | 464 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_chin.cpp` | 1153 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_chin.cpp` | 1169 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_doc.cpp` | 385 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_doc.cpp` | 649 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_doc.cpp` | 673 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_doc.cpp` | 769 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_doc.cpp` | 779 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_doc.cpp` | 811 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_doorboy.cpp` | 603 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_doorboy.cpp` | 815 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_du.cpp` | 206 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_du.cpp` | 403 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_du.cpp` | 495 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_gra.cpp` | 694 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_grc.cpp` | 496 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_grd.cpp` | 350 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_grm.cpp` | 358 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_grmc.cpp` | 432 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_gro.cpp` | 610 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_grr.cpp` | 452 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_grs.cpp` | 351 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_grz.cpp` | 697 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_grz.cpp` | 715 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_grz.cpp` | 1868 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_gwolf.cpp` | 562 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_gwolf.cpp` | 1699 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_hanjo.cpp` | 1598 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_hanjo.cpp` | 1796 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_hoz.cpp` | 559 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_ins.cpp` | 718 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_ins.cpp` | 1465 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_kasi_hana.cpp` | 1243 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_kasi_hana.cpp` | 1288 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_kasi_kyu.cpp` | 806 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_kasi_mich.cpp` | 805 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_kkri.cpp` | 576 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_moir.cpp` | 1188 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_moir.cpp` | 1485 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_moir.cpp` | 1846 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_myna2.cpp` | 844 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_ne.cpp` | 2325 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_ne.cpp` | 2536 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_ne.cpp` | 2919 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_post.cpp` | 666 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_post.cpp` | 1395 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_post.cpp` | 1413 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_prayer.cpp` | 554 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_shoe.cpp` | 510 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_taro.cpp` | 2869 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_tk.cpp` | 701 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_tks.cpp` | 1214 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_toby.cpp` | 1883 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_wrestler.cpp` | 5011 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_ykm.cpp` | 1081 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_ykm.cpp` | 1160 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_ykm.cpp` | 3204 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_ykm.cpp` | 3211 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_ykm.cpp` | 3352 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_ykw.cpp` | 719 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_ykw.cpp` | 765 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_ykw.cpp` | 2429 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_ykw.cpp` | 2939 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_zra.cpp` | 865 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_zrc.cpp` | 424 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_zrc.cpp` | 1415 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_npc_zrz.cpp` | 521 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_avalanche.cpp` | 126 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_bmWindow.cpp` | 168 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_carry.cpp` | 2607 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_carry.cpp` | 2615 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_cwall.cpp` | 451 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_cwall.cpp` | 764 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_digplace.cpp` | 137 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_digsnow.cpp` | 68 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_fchain.cpp` | 145 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_gra2.cpp` | 968 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_gra2_soldier.inc` | 272 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_kgate.cpp` | 411 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_kgate.cpp` | 496 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_kgate.cpp` | 745 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_kgate.cpp` | 794 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_kshutter.cpp` | 257 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_kshutter.cpp` | 829 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_kshutter.cpp` | 860 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_lv4digsand.cpp` | 67 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_lv6swturn.cpp` | 223 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_mirror_table.cpp` | 156 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_pdoor.cpp` | 179 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_pillar.cpp` | 276 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_rgate.cpp` | 371 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_rgate.cpp` | 448 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_rgate.cpp` | 697 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_smgdoor.cpp` | 368 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_smw_stone.cpp` | 164 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_ss_drink.cpp` | 388 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_stone.cpp` | 292 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_swchain.cpp` | 592 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_swchain.cpp` | 936 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_swchain.cpp` | 952 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_swchain.cpp` | 1045 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_swturn.cpp` | 286 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_wchain.cpp` | 270 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_wind_stone.cpp` | 143 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_obj_yobikusa.cpp` | 362 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_peru.cpp` | 352 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_peru.cpp` | 580 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_player.cpp` | 467 | Default: diverted static checkNowWolf |
| CURRENT_LINK | `src/d/actor/d_a_player.cpp` | 512 | Default: diverted static checkNowWolf |
| CURRENT_LINK | `src/d/actor/d_a_sq.cpp` | 334 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_sq.cpp` | 402 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_swc00.cpp` | 110 | Default: diverted static checkNowWolf |
| CURRENT_LINK | `src/d/actor/d_a_tag_attention.cpp` | 184 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_tag_attention.cpp` | 189 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_tag_attention.cpp` | 204 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_tag_attention.cpp` | 209 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_tag_bottle_item.cpp` | 116 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_tag_howl.cpp` | 49 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_tag_kmsg.cpp` | 99 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_tag_mwait.cpp` | 102 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_tag_ss_drink.cpp` | 162 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_tag_wara_howl.cpp` | 53 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/actor/d_a_tbox2.cpp` | 345 | Default: diverted static checkNowWolf |
| CURRENT_LINK | `src/d/actor/d_a_tbox2.cpp` | 363 | Default: diverted static checkNowWolf |
| CURRENT_LINK | `src/d/d_cc_uty.cpp` | 418 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/d_k_wmark.cpp` | 54 | Gameplay query via diverted static (context player; SP=P0) |
| CURRENT_LINK | `src/d/d_kankyo.cpp` | 2318 | Default: diverted static checkNowWolf |
| CURRENT_LINK | `src/d/d_menu_collect.cpp` | 76 | HUD / menu reflects active player |
| CURRENT_LINK | `src/d/d_menu_collect.cpp` | 2656 | HUD / menu reflects active player |
| CURRENT_LINK | `src/d/d_menu_dmap.cpp` | 1337 | HUD / menu reflects active player |
| CURRENT_LINK | `src/d/d_menu_ring.cpp` | 129 | HUD / menu reflects active player |
| CURRENT_LINK | `src/d/d_meter2.cpp` | 447 | HUD / menu reflects active player |
| CURRENT_LINK | `src/dusk/coop/coop_forms.cpp` | 171 | Default classification |
| CURRENT_LINK | `src/dusk/coop/coop_forms.cpp` | 178 | Default classification |
| CURRENT_LINK | `src/dusk/ui/editor.cpp` | 1043 | Default classification |
| CURRENT_LINK | `src/dusk/ui/touch_controls.cpp` | 255 | Co-op / PC UI |
| CURRENT_LINK | `src/f_op/f_op_actor_mng.cpp` | 887 | Gameplay query via diverted static (context player; SP=P0) |
| VIEW_OWNER | `include/d/actor/d_a_npc_cdn3.h` | 296 | Wolf eye / senses query |
| VIEW_OWNER | `include/d/actor/d_a_npc_cdn3.h` | 318 | Wolf eye / senses query |
| VIEW_OWNER | `include/d/actor/d_a_player.h` | 690 | Wolf eye / senses query |
| VIEW_OWNER | `include/d/actor/d_a_player.h` | 1204 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_gs.cpp` | 106 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_hp.cpp` | 432 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_hp.cpp` | 468 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_hp.cpp` | 898 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_hp.cpp` | 996 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_nz.cpp` | 527 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_po.cpp` | 231 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_po.cpp` | 336 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_po.cpp` | 1337 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_po.cpp` | 2469 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_po.cpp` | 2572 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_po.cpp` | 2777 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_vt.cpp` | 1781 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_vt.cpp` | 1823 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_vt.cpp` | 1862 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_vt.cpp` | 1900 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_vt.cpp` | 1952 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_vt.cpp` | 2027 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_vt.cpp` | 2105 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_vt.cpp` | 2126 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_vt.cpp` | 2193 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_vt.cpp` | 2939 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_vt.cpp` | 3323 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_vt.cpp` | 3328 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_vt.cpp` | 3339 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_ym.cpp` | 133 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_ymb.cpp` | 2585 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_e_ymb.cpp` | 3007 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_npc.cpp` | 2051 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_npc4.cpp` | 1293 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_npc_cd.cpp` | 449 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_npc_cd.cpp` | 466 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_npc_cd.cpp` | 529 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_npc_cd2.cpp` | 722 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_npc_cd2.cpp` | 735 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_npc_cd2.cpp` | 760 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_npc_cd2.cpp` | 875 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_npc_drainSol.cpp` | 185 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_npc_drainSol.cpp` | 198 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_npc_hoz.cpp` | 826 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_npc_len.cpp` | 785 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_npc_zrc.cpp` | 1384 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_obj_zra_freeze.cpp` | 129 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_player.cpp` | 474 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_a_player.cpp` | 476 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_flower.inc` | 653 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_flower.inc` | 745 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_flower.inc` | 1045 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_flower.inc` | 1217 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_grass.inc` | 569 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/actor/d_grass.inc` | 1068 | Wolf eye / senses query |
| VIEW_OWNER | `src/d/d_cam_param.cpp` | 627 | Camera / view policy |
| VIEW_OWNER | `src/d/d_camera.cpp` | 448 | Camera / view policy |
| VIEW_OWNER | `src/d/d_camera.cpp` | 1083 | Camera / view policy |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 787 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 2396 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 2427 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 2437 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 2460 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 2485 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 2490 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 2797 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 2872 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 2894 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 2923 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 2984 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 3006 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 3027 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 3043 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 3063 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 3097 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo.cpp` | 10283 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo_rain.cpp` | 6311 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo_rain.cpp` | 6604 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_kankyo_rain.cpp` | 6681 | Senses / env (per-view gated under co-op) |
| VIEW_OWNER | `src/d/d_particle.cpp` | 230 | Wolf eye / senses query |
| EVENT_PARTICIPANT | `src/d/actor/d_a_demo_item.cpp` | 370 | Event / gate / demo flow |
| EVENT_PARTICIPANT | `src/d/actor/d_a_tag_TWgate.cpp` | 325 | Event / gate / demo flow |
| EVENT_PARTICIPANT | `src/d/actor/d_a_tag_TWgate.cpp` | 531 | Event / gate / demo flow |
| EVENT_PARTICIPANT | `src/d/actor/d_a_tag_TWgate.cpp` | 583 | Event / gate / demo flow |
| EVENT_PARTICIPANT | `src/d/actor/d_a_tag_TWgate.cpp` | 728 | Event / gate / demo flow |
| EVENT_PARTICIPANT | `src/d/actor/d_a_tag_TWgate.cpp` | 781 | Event / gate / demo flow |
| EVENT_PARTICIPANT | `src/d/actor/d_a_tag_TWgate.cpp` | 921 | Event / gate / demo flow |
| EVENT_PARTICIPANT | `src/d/actor/d_a_tag_gstart.cpp` | 42 | Event / gate / demo flow |
| EVENT_PARTICIPANT | `src/d/d_event.cpp` | 241 | Event / gate / demo flow |
| EVENT_PARTICIPANT | `src/d/d_event_manager.cpp` | 465 | Event / gate / demo flow |
| EVENT_PARTICIPANT | `src/d/d_msg_flow.cpp` | 784 | Event / gate / demo flow |
| STORY_AUTHORITY | `include/d/actor/d_a_player.h` | 1167 | Standalone Midna / story Link |
| STORY_AUTHORITY | `src/d/actor/d_a_midna.cpp` | 878 | Standalone Midna / story Link |
| STORY_AUTHORITY | `src/d/actor/d_a_midna.cpp` | 1191 | Standalone Midna / story Link |
| STORY_AUTHORITY | `src/d/actor/d_a_midna.cpp` | 1215 | Standalone Midna / story Link |
| STORY_AUTHORITY | `src/d/actor/d_a_midna.cpp` | 1342 | Standalone Midna / story Link |
| STORY_AUTHORITY | `src/d/actor/d_a_midna.cpp` | 2193 | Standalone Midna / story Link |
| STORY_AUTHORITY | `src/d/actor/d_a_midna.cpp` | 2290 | Standalone Midna / story Link |
| STORY_AUTHORITY | `src/d/actor/d_a_midna.cpp` | 3208 | Standalone Midna / story Link |
| STORY_AUTHORITY | `src/d/actor/d_a_midna.cpp` | 3280 | Standalone Midna / story Link |
| GLOBAL_UNLOCK | `include/d/d_com_inf_game.h` | 1433 | Save API (get=P0 authority; set diverted) |
| GLOBAL_UNLOCK | `include/d/d_com_inf_game.h` | 1435 | Stage start / unlock form |
| GLOBAL_UNLOCK | `include/d/d_com_inf_game.h` | 1438 | Save API (get=P0 authority; set diverted) |
| GLOBAL_UNLOCK | `include/d/d_save.h` | 163 | Save read (P0 authority) |
| GLOBAL_UNLOCK | `include/dusk/coop/coop_forms_bridge.h` | 18 | Bridge |
| GLOBAL_UNLOCK | `src/d/d_com_inf_game.cpp` | 2851 | Stage start / unlock form |
| GLOBAL_UNLOCK | `src/dusk/coop/coop_debug.cpp` | 28 | Save read (P0 authority) |
| GLOBAL_UNLOCK | `src/dusk/coop/coop_forms.cpp` | 111 | Save read (P0 authority) |
| GLOBAL_UNLOCK | `src/dusk/coop/coop_forms.cpp` | 359 | Bridge |
| GLOBAL_UNLOCK | `src/dusk/coop/coop_forms.cpp` | 361 | Save read (P0 authority) |
| GLOBAL_UNLOCK | `src/dusk/ui/editor.cpp` | 1039 | Debug/editor tool |
| GLOBAL_UNLOCK | `src/dusk/ui/editor.cpp` | 1468 | Debug/editor tool |
