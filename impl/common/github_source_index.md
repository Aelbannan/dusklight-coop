# GitHub Source Index

Audited revisions: Dusklight `eed14ac`, Aurora `e3d4f82`. All links pinned to these commits.

## Dusklight core

| Component | Link |
|-----------|------|
| Game-info indexed APIs and storage | [d_com_inf_game.h](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/include/d/d_com_inf_game.h) |
| Play-scene execution/drawing | [d_s_play.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/d/d_s_play.cpp) |
| Camera implementation | [d_camera.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/d/d_camera.cpp) |
| Camera process manager | [f_op_camera_mng.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/f_op/f_op_camera_mng.cpp) |
| Draw-list/window | [d_drawlist.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/d/d_drawlist.cpp) |
| Graphics manager | [m_Do_graphic.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/m_Do/m_Do_graphic.cpp) |
| Frame interpolation | [frame_interpolation.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/dusk/frame_interpolation.cpp) |

## Input

| Component | Link |
|-----------|------|
| Dusklight controller header | [m_Do_controller_pad.h](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/include/m_Do/m_Do_controller_pad.h) |
| Dusklight controller impl | [m_Do_controller_pad.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/m_Do/m_Do_controller_pad.cpp) |
| Action bindings | [action_bindings.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/dusk/action_bindings.cpp) |
| Aurora input | [lib/input.cpp](https://github.com/encounter/aurora/blob/e3d4f82efcbc5b66ba4744b525845d189efe8dc2/lib/input.cpp) |
| Aurora PAD constants | [include/dolphin/pad.h](https://github.com/encounter/aurora/blob/e3d4f82efcbc5b66ba4744b525845d189efe8dc2/include/dolphin/pad.h) |

## Player, inventory, saves

| Component | Link |
|-----------|------|
| Link implementation | [d_a_alink.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/d/actor/d_a_alink.cpp) |
| Link damage | [d_a_alink_damage.inc](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/d/actor/d_a_alink_damage.inc) |
| Player interface | [d_a_player.h](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/include/d/actor/d_a_player.h) |
| Save structures | [d_save.h](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/include/d/d_save.h) |
| Save implementation | [d_save.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/d/d_save.cpp) |
| Item functions | [d_item.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/d/d_item.cpp) |
| Event manager | [d_event_manager.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/d/d_event_manager.cpp) |

## Form, Midna, horses

| Component | Link |
|-----------|------|
| Wolf implementation | [d_a_alink_wolf.inc](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/d/actor/d_a_alink_wolf.inc) |
| Standalone Midna | [d_a_midna.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/d/actor/d_a_midna.cpp) |
| Midna header | [d_a_midna.h](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/include/d/actor/d_a_midna.h) |
| Horseback | [d_a_alink_horse.inc](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/d/actor/d_a_alink_horse.inc) |
| Horse actor | [d_a_horse.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/d/actor/d_a_horse.cpp) |
| Horse header | [d_a_horse.h](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/include/d/actor/d_a_horse.h) |

## Combat and actors

| Component | Link |
|-----------|------|
| Attention manager | [d_attention.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/d/d_attention.cpp) |
| Collision manager | [d_cc_s.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/d/d_cc_s.cpp) |
| Actor-manager declarations | [f_op_actor_mng.h](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/include/f_op/f_op_actor_mng.h) |
| Actor-manager impl | [f_op_actor_mng.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/f_op/f_op_actor_mng.cpp) |
| ALLDIE room-clear | [d_a_alldie.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/d/actor/d_a_alldie.cpp) |
| Example enemy (Armos) | [d_a_e_ai.cpp](https://github.com/TwilitRealm/dusklight/blob/eed14acdc6b5391b9dc2ef8bed0ed3a6b8ed0371/src/d/actor/d_a_e_ai.cpp) |
