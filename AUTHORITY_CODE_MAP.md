# Authority System — Code Map

## Purpose

Concrete inventory of every transition/event hook, data boundary, and data-loss
point needed to implement co-op event authority. Generated during Step 0 of
`AUTHORITY_IMPLEMENTATION_PLAN.md`.

---

## 1. Stage-Exit Paths

### 1.1 Scene Exit Actor (daScex_c)

| File | Function | Key Lines |
|------|----------|-----------|
| `src/d/actor/d_a_scene_exit.cpp` | `daScex_c::execute()` | L80–L110 |
| `src/d/actor/d_a_scene_exit2.cpp` | `daScex2_c::execute()` | (variant) |

**Flow:**
1. `checkWork()` — evaluates event bits / switches.
2. `daPy_getPlayerActorClass()` — **always P1/vanilla player** (no co-op resolution).
3. `player->onSceneChangeArea(getArg0(), getPathID(), this)` — notifies Link.
4. Link stores `mExitID` and sets state for its main proc.
5. Later: `daAlink_c` main proc → `dStage_changeScene()` → `dComIfGp_setNextStage()`.
6. `dStage_nextStage_c::set()` sets `enabled = true`.
7. `dScnPly_Execute()` consumes enabled next stage (see §1.3).

**Initiator:** Lost between step 3 and 5. The exit actor has no player ID, and
`daPy_getPlayerActorClass()` hardcodes player 0. By the time
`dStage_changeScene()` is called, the triggering player's identity is gone.

**Anchor:** `current.pos` of the exit actor (scaled bounding box: 75×150×75).

**Next-stage params:** `getArg0()` = exit ID → `onSceneChangeArea(exitID)` →
`mExitID` → `dStage_changeScene(mExitID, ...)` → `dComIfGp_setNextStage(...)`.
The exit ID resolves via `dStage_changeScene()` into
`stage_scls_info_class` which provides target stage name, room, point, layer,
wipe, and wipe speed.

### 1.2 Link Direct Scene-Change Calls

| File | Function | Exit Used |
|------|----------|-----------|
| `src/d/actor/d_a_alink.cpp` | `daAlink_c::checkSceneChange()` (L14124) | `mExitID & 0xFF` or ceiling ground |
| `src/d/actor/d_a_alink.cpp` | `daAlink_c::startPeepChange()` (L13739) | `mPeepExitID` |
| `src/d/actor/d_a_alink.cpp` | `daAlink_c::procCoLavaReturnInit()` | `dStage_changeScene(exitID, ...)` → L13956 |
| `src/d/actor/d_a_alink.cpp` | `daAlink_c::onSceneChangeArea()` | stores `mExitID` |
| `src/d/actor/d_a_alink.cpp` | `daAlink_c::onSceneChangeAreaJump()` | variant |

**Path from `onSceneChangeArea`:**
```
daScex_c::execute()
  → player->onSceneChangeArea(exitID, pathID, exitActor)
  → daAlink_c::onSceneChangeArea()        // stores mExitID, mpScnChg
  → later in main proc:
     if (eventInfo.checkCommandDoor() || ...)
       dStage_changeScene(mExitID & 0xFF, ...)
         → dComIfGp_setNextStage(...)
           → dStage_nextStage_c::set(...) // enabled=true
```

**Data loss:** The requesting player's identity is gone after
`onSceneChangeArea()`. The transition fires from the Link's own main proc with
no context about whether it was P2 who walked into the trigger.

### 1.3 Next-Stage Consumption

| File | Function | Key Lines |
|------|----------|-----------|
| `src/d/d_stage.cpp` | `dStage_nextStage_c::set()` | L32–L47 |
| `src/d/d_stage.cpp` | `dStage_changeScene()` | L2851–L2886 |
| `src/d/d_stage.cpp` | `dStage_changeScene4Event()` | L2896–L2935 |
| `src/d/d_stage.cpp` | `dStage_changeSceneExitId()` | L2846–L2848 |
| `src/d/d_s_play.cpp` | `dScnPly_Execute()` | L727+ |

**Key detail:** `dStage_nextStage_c::set()` only transitions if `!enabled`.
Once set, subsequent calls are silently ignored — this is a natural
deduplication point. `offEnable()` resets it.

**Boundary for gating:** `dStage_changeScene()` calls
`dComIfGp_setNextStage()` which calls `dStage_nextStage_c::set()`. If we want
to defer the transition, we must intercept before this write. The
`dStage_changeScene()` return value indicates success (1) or failure (0).

### 1.4 Other Direct SetNextStage Callers

| File | Function | Purpose |
|------|----------|---------|
| `src/d/d_com_inf_game.cpp` | `dComIfGp_setNextStage()` | death/game-over restart (L1225) |
| `src/d/d_menu_quit.cpp` | `(anonymous)` | quit to menu (L64) |
| `src/d/d_s_menu.cpp` | multiple | menu → stage transitions |
| `src/d/d_s_logo.cpp` | `(anonymous)` | logo → title (L819) |
| `src/d/d_s_name.cpp` | `(anonymous)` | name entry → game (L413) |
| `src/dusk/ui/warp.cpp` | warp handler | warp menu (L325) |
| `src/dusk/imgui/ImGuiStateShare.cpp` | state share | debug warp (L162) |

These are **menu/logo/system transitions** and must NOT be gated by party
readiness.

---

## 2. Event-Order Entry Points

### 2.1 fopAcM_Order* Functions

| Function | File | Request Actor | Target Actor | Issue |
|----------|------|---------------|--------------|-------|
| `fopAcM_orderTalkEvent()` | `src/f_op/f_op_actor_mng.cpp:1113` | caller `i_actorA` (Link) | `i_actorB` (NPC) | ✅ Initator preserved |
| `fopAcM_orderSpeakEvent()` | `src/f_op/f_op_actor_mng.cpp:1143` | **`dComIfGp_getPlayer(0)`** | `i_actor` (NPC/obj) | ❌ Initator LOST — always P1 |
| `fopAcM_orderOtherEventId()` | `src/f_op/f_op_actor_mng.cpp:1260` | `i_actor` | **`event_second_actor(i_flag)` = P1** | ❌ Second actor always P1 |
| `fopAcM_orderMapToolEvent()` | `src/f_op/f_op_actor_mng.cpp:1295` | `i_actor` | **`event_second_actor(i_flag)` = P1** | ❌ Second actor always P1 |
| `fopAcM_orderDoorEvent()` | `src/f_op/f_op_actor_mng.cpp:1194` | `i_actorA` (Link) | `i_actorB` (door) | ✅ Both preserved |
| `fopAcM_orderItemEvent()` | `src/f_op/f_op_actor_mng.cpp:1252` | **`dComIfGp_getPlayer(0)`** | `i_actor` | ❌ Initator LOST |
| `fopAcM_orderTreasureEvent()` | `src/f_op/f_op_actor_mng.cpp:1274` | `i_actorA` (Link) | `i_actorB` (chest) | ✅ Both preserved |
| `fopAcM_orderCatchEvent()` | `src/f_op/f_op_actor_mng.cpp:1208` | `i_actorA` (Link) | `i_actorB` | ✅ Both preserved |
| `fopAcM_orderPotentialEvent()` | `src/f_op/f_op_actor_mng.cpp:1242` | `i_actor` | `event_second_actor(i_flag)` = P1 | ⚠️ Second actor is P1 |

**Critical observation:** `event_second_actor()` (L1099–L1102) is defined as:
```cpp
void* event_second_actor(u16 i_flag) {
    (void)i_flag;
    return dComIfGp_getPlayer(0);
}
```

This is a **hardcoded P1 assumption** that infects all OTHER-type event orders.
The `setParam()` call in `dEvt_control_c` then uses `order->mpRequestActor`
and `order->mpTargetActor` to set `Pt1` and `Pt2`. So the event manager's
participant records are wrong for non-P1 initiators.

### 2.2 Callers in daAlink_c (Conversations)

| Caller Location | Function Called | Context |
|----------------|----------------|---------|
| `orderTalk()` (L11677) | `fopAcM_orderTalkEvent(this, field_0x27f4, 0, 0)` | ✅ `this` = Link, `field_0x27f4` = NPC |
| `orderZTalk()` (L11734) | `fopAcM_orderTalkEvent(this, zhint, 0, 0)` | Midna/zHint interaction |
| `orderZTalk()` (L11765) | `fopAcM_orderOtherEventId(zhint, eventID, toolID, ...)` | zHint event |
| `orderZTalk()` (L11774) | `fopAcM_orderTalkEvent(this, getMidnaActor(), 0, 0)` | Midna talk |
| `orderPeep()` (L11664) | `fopAcM_orderOtherEvent(this, field_0x27f4, l_peepEventName, ...)` | Peep-hole event |
| interaction update (L11581) | `orderZTalk()` | Wolf grab → talk |
| item trade | `fopAcM_orderTalkItemBtnEvent(...)` | X/Y trade items |

For talk events, the initiating Link (`this`) is correctly passed as the
request actor. The target actor is the NPC/object. So **`fopAcM_orderTalkEvent`**
does preserve the Link context — the issue is in `fopAcM_orderSpeakEvent` and
`event_second_actor`.

### 2.3 Other Actor Event Sources

| Actor | File | Event Call |
|-------|------|------------|
| `daTag_Event_c` | `src/d/actor/d_a_tag_event.cpp` | `getEventIdx(this, getEventNo())` → later `order()` via eventInfo |
| `daNpc_Tks_c` (Telma) | `src/d/actor/d_a_npc_tks.cpp` | multiple `fopAcM_orderOtherEventId()` |
| `daNpc_Hoz_c` (Auru) | `src/d/actor/d_a_npc_hoz.cpp` | `fopAcM_orderOtherEventId()` |
| `daNpc_Bou_c` (Boulder) | `src/d/actor/d_a_npc_bou.cpp` | `fopAcM_orderSpeakEvent()` |
| `daObj_Ladder_c` | `src/d/actor/d_a_obj_ladder.cpp` | `fopAcM_orderOtherEventId(this, mEventIdx, prm_get_evId(), ...)` |
| `daObj_Scannon_c` | `src/d/actor/d_a_obj_scannon.cpp` | `fopAcM_orderOtherEventId(this, ...)` |
| `daObj_DmElevator_c` | `src/d/actor/d_a_obj_dmelevator.cpp` | `fopAcM_orderOtherEventId(this, ...)` |
| `daTag_Mwait_c` | `src/d/actor/d_a_tag_mwait.cpp` | `fopAcM_orderSpeakEvent(this, 0, 0)` — caller is tag, not Link |
| `daAlldie_c` | `src/d/actor/d_a_alldie.cpp` | `fopAcM_orderOtherEventId(this, ...)` |
| `daTag_Kmsg_c` | `src/d/actor/d_a_tag_kmsg.cpp` | `fopAcM_orderOtherEventId(this, ...)` |
| `daTag_Hstop_c` | `src/d/actor/d_a_tag_hstop.cpp` | `fopAc
### 2.4 Event Manager Entry

`dEvent_manager_c::orderStartDemo()` (L442 in `d_event_manager.cpp`) — called
once by P1 during Link create. Already guarded by co-op's
`isStoryAuthorityLink()`. The start-demo request is queued into the event
manager and processed during `Sequencer()` / `Step()`.

**Phase 2 integration point:** The `orderStartDemo()` call must be deferred
until secondary Links have finished creating. Currently runs at create time
with no awareness of pending secondary Links.

---

## 3. Conversation Path (Data Loss Analysis)

### 3.1 `fopAcM_orderSpeakEvent()` — Primary Data Loss Point

```
Caller (NPC/tag actor) calls fopAcM_orderSpeakEvent(this, 0, 0)
  → requestActor = dComIfGp_getPlayer(0)    ← ALWAYS P1
  → targetActor = i_actor (the NPC/tag)
```

The `requestActor` should be the Link that is currently near the NPC and
pressing A. Instead, it's hardcoded to P1. This means:
- `dEvt_control_c::setParam()` sets `mPt1 = P1` even if P2 is talking.
- `dComIfGp_event_getPt1()` returns P1.
- `fopAcM_getTalkEventPartner()` returns P1.
- The event system believes P1 is the speaker.
- Any "give item to speaker" reward goes to P1.

**Callers affected:**
- `daNpc_Bou_c` (Boulder NPC) — `fopAcM_orderSpeakEvent(this, 0, 0)`
- `daTag_Mwait_c` (Midna wait) — `fopAcM_orderSpeakEvent(this, 0, 0)`
- `daTag_Hstop_c` (horse stop) — `fopAcM_orderSpeakEvent(this, 0, 0)`
- `daNpc_Tks_c` (Telma) — `fopAcM_orderSpeakEvent(this, 0, 0)`
- `daNpc_Hoz_c` (Hena) — `fopAcM_orderSpeakEvent(this, 0, 0)`

### 3.2 `event_second_actor()` — Secondary Data Loss

```
void* event_second_actor(u16 i_flag) {
    (void)i_flag;
    return dComIfGp_getPlayer(0);
}
```

Called by all `fopAcM_orderOtherEvent*()` and `fopAcM_orderMapToolEvent*()`
functions. Sets the target participant to P1 instead of the actual initiator.

### 3.3 Conversation Rewards

Vanilla path: event callback → `dComIfGp_event_getPt1()` /
`dComIfGp_event_getTalkPartner()` → reward applies to P1.

**Fix required:** Replace `dComIfGp_getPlayer(0)` calls in the reward path
with the actual initiator from co-op event metadata.

---

## 4. First Party-Story Event Identification

The first route-blocking NPC in Twilight Princess is context-dependent (first
story gate in Ordon Village → Faron Woods). The exact event and actor cannot
be named statically because the game uses data-driven event placement per
stage file. **Runtime discovery is required** using a debug log of event IDs
when the first NPC interaction occurs.

Key candidates:
- Ordon Village: Sword training (F_SP101), goat herding, shield tutorial
- Faron Woods: Monkey rescue, first corridor
- These use `daNpc_c` subclasses (Kolin, Bou, etc.) which call
  `fopAcM_orderSpeakEvent()` or `fopAcM_orderTalkEvent()`

---

## 5. Debug Instrumentation Added (Step 0)

### 5.1 `src/d/actor/d_a_scene_exit.cpp`

| Line | Instrumentation |
|------|----------------|
| L13–15 | `#if TARGET_PC` include guard |
| L71–83 | `logInfo()` on exit trigger with exitID, pathID, arg1, actor pos, scale |

**Records:** The exit actor's identity, position, and type when a player
enters the trigger volume. Does NOT record which player triggered it
because the vanilla code only uses `daPy_getPlayerActorClass()`.

### 5.2 `src/d/d_stage.cpp`

| Line | Instrumentation |
|------|----------------|
| L26 | `#include "dusk/coop/coop_debug.h"` |
| L2887–2895 | `logInfo()` in `dStage_changeScene()` with exitID, target stage name, room, point |
| L158–166 | `logInfo()` in `dStage_nextStage_c::set()` with stage name, room, point, layer, wipe |

**Records:** The exact transition target when a scene change is initiated and
when it is committed (enabled flag set).

### 5.3 `src/d/d_event.cpp`

| Line | Instrumentation |
|------|----------------|
| L255–266 | `logInfo()` in `dEvt_control_c::order()` with event type, priority, flags, request/target actor names, event ID, map tool ID |
| L370–382 | `logInfo()` in `talkCheck()` with request/target actor names and event ID |

**Records:** Every event order and conversation start with full actor
identification.

### 5.4 `src/f_op/f_op_actor_mng.cpp`

| Line | Instrumentation |
|------|----------------|
| L34 | `#include "dusk/coop/coop_debug.h"` |
| L1145–1152 | `logInfo()` in `fopAcM_orderSpeakEvent()` recording the **data loss** — initiator is hardcoded P1 |

**Records:** Every `orderSpeakEvent` call, explicitly noting the initiator
is always P1.

---

## 6. Build Verification

The added instrumentation is:
- Guarded by `#if TARGET_PC` — zero impact on non-PC builds
- Read-only logging — no behavior changes
- Uses existing `dusk::coop::debug::logInfo()` infrastructure

**Known include dependency for game files:**
- `dusk/coop/coop_debug.h` is required for `debug::logInfo()`
- `d/d_stage.h` is required for `dStage_getName()` (already transitively
  included by most game files)

---

## 7. Unresolved Issues

1. **First party-story event identity**: Cannot be statically determined.
   Mark `AUTHORITY_IMPLEMENTATION_PLAN.md` Phase 4 as requiring runtime
   discovery. The debug logs added in Step 0 will reveal the concrete event
   ID on first playthrough.

2. **`dComIfGp_getPlayer(0)` in `event_second_actor()`**: This is the root
   cause of conversation attribution loss. Fixing it requires a ScopedContext
   or similar per-player tracking in `event_second_actor()`. Deferred to
   Phase 5.

3. **Secondary Link start-demo timing**: The existing P1-only guard
   (`isStoryAuthorityLink`) suppresses secondary start demos, but the
   `orderStartDemo()` call happens during P1 create before secondary Links
   are spawned. Phase 2 must defer this pending the party barrier.

4. **`dStage_changeScene()` return value**: Currently returns 1 on success.
   A gating interceptor must return 0 to prevent the transition while the
   party barrier is active, then call the real function when ready.

5. **Phase 7 classification needed**: Doors, treasures, item pickups, warps,
   minigames are not yet classified for scope/authority. See
   `AUTHORITY_IMPLEMENTATION_PLAN.md` Phase 7 for the classification table
   template.

---

## 8. Next-Stage Boundary Summary

```
daScex_c::execute()       ← logged (no player context)
  → onSceneChangeArea()   ← stores mExitID
  → dStage_changeScene()  ← logged, records target stage
    → dComIfGp_setNextStage()
      → dStage_nextStage_c::set()  ← logged, records enable
        → enabled=true
  → dScnPly_Execute()     ← consumes enabled, does transition
  
GATE POINT: Between dStage_changeScene() returning 1 and the transition
actually consuming the enabled flag, we must intercept to check party
readiness. If not ready, suppress the set() and save the params for later.
```
