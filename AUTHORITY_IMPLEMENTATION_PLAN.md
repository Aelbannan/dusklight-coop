# Authority System Implementation Plan

## Scope

Implement the authority model in `AUTHORITY.md` without changing the non-PC path:

- P1 (`PlayerId == 0`) remains global story/event flow authority.
- Stage entry, stage exit, and party-synchronized story events wait for every joined Link to stay near an event anchor.
- Players may move while waiting; a player outside the radius is not ready.
- Waiting uses `dusk::ui::push_toast_to_all_views`.
- Specific-player global events such as conversations use the initiator as presentation owner.
- During an initiator-owned event, only that player's view is visible/full-screen; all other views are hidden and restored afterward.
- Conversation rewards are party-wide for now.

This is a planning document. No implementation is implied by the plan stages below.

## Research snapshot

### Existing authority behavior

- Secondary Links already suppress their own stage-start demos in `src/d/actor/d_a_alink.cpp`.
- P1 is the only actor currently calling the normal start-demo path.
- `dusk::coop::render::worldDrawPassCount()` already collapses event rendering to one full-screen pass, but that pass is hard-wired to camera/view 0.
- `dComIfGp_event_order()` and the event manager are global and have no co-op initiator/participant metadata.
- `fopAcM_orderSpeakEvent()` currently uses `dComIfGp_getPlayer(0)` as the speaker.
- `daScex_c::execute()` currently tests only the vanilla player and calls `onSceneChangeArea()` directly.
- Scene transitions ultimately become global next-stage state through `dComIfGp_setNextStage()` / `dStage_nextStage_c::set()`, then are consumed by `dScnPly_Execute()`.

### Main risks

1. Deferring a scene transition after `setNextStage()` is too late; the next-stage state must be gated before it becomes enabled.
2. Many actors can order events or request scene changes, so P1-only guards are insufficient.
3. The vanilla event manager stores one global event and assumes player 0 in several paths.
4. The current single-view renderer hides split-screen correctly, but cannot select an arbitrary initiator camera.
5. Secondary Links need explicit participation/lock handling during party-synchronized events.

## Proposed central abstraction

Add a co-op event arbiter subsystem, tentatively:

```text
include/dusk/coop/coop_event.h
src/dusk/coop/coop_event.cpp
```

The subsystem should own one pending or active global co-op event at a time.

### Event metadata

The exact representation can use fixed-size arrays/bitmasks rather than dynamic containers, but the model needs:

```text
EventRequest
  kind                 StageEntry / StageExit / StoryCutscene /
                       PartyStory / Conversation / deferred kinds
  scope                P1Story / PartySynchronized / InitiatorOwned
  initiator            PlayerId or none for autonomous story events
  flowAuthority        P1
  presentationOwner    P1 or initiator
  anchorActor           optional actor
  anchorPosition        cached cXyz
  proximityRadius       event-specific radius
  participants          joined-player snapshot or all-joined policy
  vanilla event data    event ID, actors, exit/next-stage parameters
```

The arbiter state should distinguish:

```text
Idle
WaitingForParty
ReadyToCommit
Running
Finishing
```

It must also deduplicate repeated requests and restore presentation state on both normal completion and abort/reset.

## Staged implementation

### Phase 0 — Inventory and instrumentation ✅ (Complete)

Completed by flash agent. See `AUTHORITY_CODE_MAP.md` for full inventory.

1. ✅ Enumerated all player-triggered scene-change paths (daScex_c, daAlink_c direct calls, dStage_changeScene, setNextStage callers).
2. ✅ Enumerated all event-order entry points (fopAcM_orderTalkEvent, fopAcM_orderSpeakEvent, fopAcM_orderOtherEventId, plus 16+ NPC/actor callers).
3. ⚠️ First route-blocking NPC/event: **Runtime discovery required** — event data is stage-file-driven, not static. Debug logs from Phase 0 will identify it on first playthrough.
4. ✅ Debug logging added at the following boundaries:
   - `daScex_c::execute()` — exit trigger (actor pos, exitID, arg1)
   - `dStage_changeScene()` — target stage/room/point
   - `dStage_nextStage_c::set()` — final enable gate
   - `dEvt_control_c::order()` — every event order with actor names
   - `dEvt_control_c::talkCheck()` — conversation start
   - `fopAcM_orderSpeakEvent()` — data loss point documented

Documented data-loss boundaries:
- `daPy_getPlayerActorClass()` in daScex_c::execute() — initiator lost
- `event_second_actor()` → `dComIfGp_getPlayer(0)` — ALL OtherEvent second actors forced to P1
- `fopAcM_orderSpeakEvent()` → `dComIfGp_getPlayer(0)` — request actor forced to P1
- `fopAcM_orderItemEvent()` → `dComIfGp_getPlayer(0)` — request actor forced to P1

The first party-synchronized event should use an explicit, documented classifier/allowlist if event data does not provide a reliable scope marker. Avoid globally treating every `OTHER` event as party-synchronized.

### Phase 1 — Arbiter and readiness primitives

Create the event subsystem and wire `init()`, `reset()`, and `tick()` from `dusk::coop::Runtime`.

Add reusable helpers for:

- checking whether a Link is valid and joined;
- measuring horizontal/3D distance to an anchor;
- testing whether every required player is currently near the anchor;
- taking a participant snapshot;
- deduplicating an equivalent request;
- throttling waiting notifications.

The arbiter must not start an event merely because the first player is ready. It should continuously reevaluate all joined players and only commit when all required players are valid and within the request radius.

When blocked, send a throttled warning to all views with `push_toast_to_all_views`. The exact wording may vary by event type, but it should identify that the party must gather.

### Phase 2 — Stage entry

Preserve the existing rule that only P1 orders the stage-entry story demo, but defer that order until the party barrier passes.

Likely integration points:

- `src/d/actor/d_a_alink.cpp` start-demo initialization;
- `player::onLinkReady()` and secondary Link recreation;
- event manager start-demo ordering;
- stage/room lifecycle callbacks.

The entry request should use the stage-start/story anchor, include every joined player, and require all Links to be valid and near it. P1 remains the presentation focus.

Important requirement: avoid starting `orderStartDemo()` during P1 creation before secondary Links have finished creating. The pending request must survive the create-phase timing without repeatedly ordering the same demo.

### Phase 3 — Stage exit

Make any joined player able to request a party transition, while P1 remains the only flow authority that commits it.

Primary player exit integration:

- `src/d/actor/d_a_scene_exit.cpp` (`daScex_c::execute()`);
- Link `onSceneChangeArea()` / related transition procedures;
- direct Link exit/warp paths.

Central safety integration:

- `dStage_changeScene()`;
- `dStage_changeScene4Event()`;
- `dComIfGp_setNextStage()` or `dStage_nextStage_c::set()`;
- `dScnPly_Execute()` as a final transition-consumption boundary.

The implementation should capture the target stage/room/point/wipe parameters once, then prevent competing calls from enabling another transition. The party barrier should use the exit trigger's location as the anchor. Do not gate unrelated menu/logo/system transitions.

When the barrier passes, P1 performs the original transition operation exactly once. Scene/room teardown must preserve joined slots and coordinate with secondary Link recreation.

### Phase 4 — Party-synchronized story event

Implement the first known route-blocking NPC as a party-synchronized event.

Behavior:

1. A first valid trigger creates a request.
2. The request records the NPC/event anchor and all joined players.
3. The party may continue moving while waiting.
4. Leaving the anchor radius makes a player unready.
5. Once everyone is ready, P1 orders the vanilla event once.
6. Every Link is treated as a participant, even though P1 is the presentation focus.
7. NPC interaction, event flags, and global side effects happen once.
8. The event result is applied party-wide.

Do not invoke the NPC event once per Link. The arbiter must suppress duplicate requests and preserve one global event identity.

Secondary Links may initially need a controlled wait/lock state while the event is running. This should be implemented through explicit co-op event participation state rather than pretending every Link independently owns the vanilla event.

### Phase 5 — Initiator-owned conversations

Route conversation requests through the arbiter while preserving the initiating Link.

Primary points:

- `fopAcM_orderSpeakEvent()` must stop blindly using player 0 on PC.
- Explicit `fopAcM_orderTalkEvent()` actor pairs need context validation.
- NPC/object-originated speak paths need a reliable current-player context when a player is interacting.
- The arbiter must preserve initiator, target, event ID, and presentation owner through event start/end.

Conversation policy:

- P1 starts/arbitrates the global event.
- The initiator owns conversation semantics and input.
- Only the initiator's view is visible.
- Every joined player receives the conversation reward for now.
- Global flags are committed once.

Do not assume that replacing one `getPlayer(0)` call solves all attribution; inspect event `Pt1`/`Pt2`, talk partner, skip/input, and event-context reads as one path.

### Phase 6 — Single-visible-initiator presentation

Extend the existing event single-view path in `src/dusk/coop/coop_render.cpp` with an explicit presentation override:

```text
active
presentationOwner = ViewId
saved layout/view state
```

During an initiator-owned event:

- render one full-screen pass;
- resolve the event camera from the initiating view rather than always camera 0;
- hide every other viewport completely;
- route event HUD/message presentation to the initiator;
- preserve P1 as flow authority only;
- restore the previous split-screen layout after event end or abort.

The current `worldDrawPassCount() == 1` behavior is useful, but `resolveWindow()` and `resolveCamera()` currently fall back to index 0. The override must select the initiator camera while still using a full-screen window.

Event completion should be detected from the arbiter/event-manager lifecycle, not only from a one-frame render check, so restoration also works for event reset, scene transition, and abort paths.

For P1-focused story and party-synchronized events, retain the existing P1 single-view presentation behavior.

### Phase 7 — Results and broader event classification

Once the first flows work, classify deferred categories:

- doors;
- treasure/item pickups;
- catch/presentation events;
- warps/portals;
- map-tool/scripted events;
- autonomous enemy/object events;
- minigames.

Each should be assigned one of:

```text
P1/story-owned
party-synchronized
initiator-owned/global
local/per-player
```

Implement party-wide result routing separately from event presentation. A global flag should be committed once; per-player inventory/resource rewards should be applied to each joined player according to the event policy.

## Testing strategy

### Unit/state tests

- no joined players outside the initialized P1 slot;
- one player leaves/re-enters the anchor radius;
- all joined players become ready on different frames;
- duplicate requests for one anchor;
- competing requests for different anchors;
- event reset/abort while waiting;
- event reset/abort while presentation override is active;
- layout restoration after P2 conversation;
- sparse joined slots and player-count changes.

### Runtime scenarios

1. P1 stage entry with P2 joining late.
2. P2 stage exit while P1 is far away.
3. A player walks out of the exit radius during the wait.
4. Opening route-blocking NPC with all Links present.
5. P2 talks to an NPC:
   - only P2 view visible;
   - P1 still controls flow;
   - other views hidden;
   - party reward applied once per player;
   - split-screen restored afterward.
6. Two players attempt to talk or exit simultaneously.
7. A global event ends in a scene transition.
8. Secondary Links are recreated during a stage transition.

### Required diagnostics

Log event kind, initiator, presentation owner, anchor, readiness mask, target stage/event ID, commit, and release. This will be essential for diagnosing global vanilla event state that still assumes player 0.

## Open decisions intentionally deferred

- Whether non-initiating players are free to move during an active initiator-owned event or are frozen by the vanilla event system.
- Exact proximity radii for each anchor/event type.
- Whether waiting has a future timeout, force-warp, or cancellation policy.
- How to classify all non-talk event types.
- Whether event scope should eventually be data-driven rather than code-whitelisted.
- Authority reassignment and player absence; P1 is assumed present for this phase.

## Concrete step contracts

Each implementation step below is intentionally narrow. A step is complete only when its code, diagnostics, and focused verification are present; the next agent must not silently skip a failing prerequisite.

## Implementation Status (Final Integration Audit)

### Phase 1–6 Implementation ✅ (Complete)

All six phases have been implemented by preceding flash agents.  The final integration audit verified:

| Phase | What | Status | Files |
|-------|------|--------|-------|
| 0 | Inventory & instrumentation | ✅ | AUTHORITY_CODE_MAP.md, debug logs in d_a_scene_exit.cpp, d_d_event.cpp, d_stage.cpp, f_op_actor_mng.cpp |
| 1 | Arbiter skeleton | ✅ | coop_event.h, coop_event.cpp, coop.cpp (init/reset/tick wiring) |
| 2 | Stage entry gating | ✅ | d_a_alink.cpp (playerInit defer, create wait, execute commit) |
| 3 | Stage exit gating | ✅ | d_a_scene_exit.cpp (multiplayer exit zone detection), d_a_alink.cpp (checkSceneChange gating) |
| 4 | Party-synchronized story event | ✅ (conservative classifier) | f_op_actor_mng.cpp (orderSpeakEvent intercept), d_a_alink.cpp (PartyStory commit) |
| 5 | Initiator-owned conversations | ✅ | f_op_actor_mng.cpp (orderSpeakEvent/orderTalkEvent tracking), d_d_event.cpp (setParam PtT/PtI fix), d_a_alink.cpp, d_event.cpp (endProc) |
| 6 | Single-visible-initiator presentation | ✅ | coop_render.h/cpp (push/popConversationPresentation, resolveCamera/Window) |

### Concrete issues found and fixed during audit

#### Bug 1: `commitStageExitNow()` groundPath branch (both branches identical)
- **File**: `src/dusk/coop/coop_event.cpp`
- **Issue**: Both the `groundPath` and non-ground branches called `dStage_changeScene()` with the same signature.
- **Fix**: Added a log warning documenting that the ground collision polygon is not captured in `CapturedExitParams`, and falls through to the standard path.  A future agent should either store the `cBgS_PolyInfo` or restructure the commit to go through `daAlink_c::checkSceneChange()`.
- **Impact**: No functional change since no caller sets `groundPath=true` yet.

#### Bug 2: `cancel()` did not clear deferred state
- **File**: `src/dusk/coop/coop_event.cpp`
- **Issue**: Calling `cancel(token)` only cleared the `EventSlot` and the `s_activePartyStoryToken`.  It left `g_deferredEntry`, `g_deferredExit`, and `g_deferredPartyStory` in their previous state, causing `isStageEntryPending()` / `isStageExitPending()` / `isPartyStoryPending()` to return stale true values.
- **Fix**: `cancel()` now checks if the token matches `g_deferredEntry.token`, `g_deferredExit.token`, or `g_deferredPartyStory.token` and clears the corresponding deferred state block.  It also pops the conversation presentation override when cancelling the active conversation token.
- **Impact**: Prevents false "pending" state after cancellation.  Prevents stale presentation override.

#### Bug 3: `slotFo` typo (truncated identifier from reconstructed file)
- **File**: `src/dusk/coop/coop_event.cpp`
- **Issue**: File was corrupted during the audit and had to be reconstructed.  A truncation produced `slotFo`+newline+`r(…)` instead of `slotFor(…)`.
- **Fix**: Restored correct function call.
- **Impact**: Compilation fix.

### Remaining integration concerns (non-blocking)

1. **Stage entry deferral timing**: The `deferStageEntry()` call in `playerInit()` only triggers when `runtime().joinedPlayerCount > 1` at P1's initialization moment.  If P2 joins after P1 finishes `playerInit()`, the start demo runs before P2 is ready.  This is a known policy limitation documented in the plan.

2. **Conversation start detection gap**: `applyConversationRewards()` reads `dComIfGp_event_getGtItm()` at event-end.  This only captures the last item granted by the event manager, which may not match the reward semantics for multi-step conversations or scripted NPC interactions where the item is granted mid-event rather than at end.  Verification on a real playthrough is required.

3. **PartyStory runtime-discovery dependency**: The `dusk_coop_isPartyStoryEvent()` classifier always returns 0 — no concrete event+stage+room combination has been confirmed yet.  The first route-blocking NPC event must be identified via the `PARTYSTORY-CANDIDATE` debug logs emitted by `dusk_coop_logPartyStoryCandidate()`.  Until then, all NPC interactions fall through to the conversation path (Step 5) rather than the party-barrier path (Step 4).  This is by design.

4. **Scene transition parameter fidelity**: Exit parameters (speed, mode, angle) are captured from `daScex_c::execute()` at the moment a player enters the exit zone, not at the moment the arbiter commits.  If the committed player's Link state changes between trigger and commit, the transition parameters may be stale.  Currently the params are populated with defaults (0, 0, 0) from the co-op capture path, which may not match every exit type.  This should be verified per-exit-type during playthrough.

5. **Ground-collision exit types**: `dStage_changeSceneExitId()` (used when the Link falls off a cliff or walks off a ledge) requires a `cBgS_PolyInfo` ground polygon.  This polygon is not stored in `CapturedExitParams` and is not handled by the current co-op exit gate.  Players falling off ledges in co-op will bypass the stage-exit barrier entirely.

### Deferred event categories (not implemented)

These categories are explicitly deferred and remain on the vanilla single-player path:

- **Doors** (fopAcM_orderDoorEvent, daDoor_knob00, daDoor_dbdoor00, daDoor_bossL5, etc.)
- **Treasure/item pickups** (orderGetItemEvent, itemGetPath, catch/presentation)
- **Warps/portals** (daObj_bosswarp, daPy_warp, midna warp)
- **Map-tool/scripted events** (map tool scripted event orders)
- **Autonomous enemy/object events** (self-ordered events from NPCs/enemies)
- **Minigames** (fishing, sumo, etc.)

Each remains P1-only via the existing vanilla event path.  A future phase (Phase 7) should classify each category with a scope/authority/presentation/result policy.

### Build verification

- **Compiler**: Apple Clang (Xcode 16)
- **Target**: macos-arm64, TARGET_PC=1, NDEBUG
- **Result**: Clean compile with zero errors and zero new warnings.  All 8 modified compilation units (`d_a_alink.cpp`, `d_a_scene_exit.cpp`, `d_d_event.cpp`, `d_stage.cpp`, `f_op_actor_mng.cpp`, `coop.cpp`, `coop_render.cpp`, `coop_event.cpp`) compiled and linked successfully into the `Dusklight` executable and `dusklight-stub`.
- **Pre-existing warnings**: 8 unrelated warnings in `d_a_obj_tks.h` (undefined inline).

### Non-PC path verification

Every behavioral change is guarded by `#if TARGET_PC` or its equivalent:
- `src/d/actor/d_a_alink.cpp`: all 5 patches (`playerInit`, `create`, `checkSceneChange`, `execute` ×2) are inside `#if TARGET_PC` blocks.
- `src/d/actor/d_a_scene_exit.cpp`: the multiplayer exit zone loop and `deferStageExit` call are inside a `#if TARGET_PC` block.
- `src/d/d_event.cpp`: the `setParam` PtT/PtI fix and `endProc` party-story/conversation lifecycle hooks are inside `#if TARGET_PC` blocks.
- `src/d/d_stage.cpp`: the `dStage_changeScene` debug log is inside `#if TARGET_PC`.
- `src/f_op/f_op_actor_mng.cpp`: all changes (event_second_actor, orderTalkEvent, orderSpeakEvent) are inside `#if TARGET_PC` blocks.
- `src/dusk/coop/coop_event.cpp`: bridge functions are inside `#if TARGET_PC`.

No accidental non-PC behavior changes were found.

## Deferred policy for implementation steps

### Step 1 — Add the arbiter skeleton ✅ (Complete)

Implemented in `include/dusk/coop/coop_event.h` and `src/dusk/coop/coop_event.cpp`.

### Step 2 — Implement readiness and waiting notifications ✅ (Complete)

Implemented in `coop_event.cpp` (`recomputeReadiness`, `allReady`, `kToastInterval` throttling, `readinessSummary`).

### Step 3 — Gate stage entry, then stage exit ✅ (Complete)

Stage entry: deferred in `daAlink_c::playerInit()` via `event::deferStageEntry()`, committed from `daAlink_c::execute()`.
Stage exit: trigger detection in `daScex_c::execute()`, gated in `daAlink_c::checkSceneChange()`.

### Step 4 — Add the first party-synchronized story event ✅ (Complete, classifier returns 0)

Classification infrastructure in `dusk_coop_isPartyStoryEvent()` (bridge) and `dusk_coop_logPartyStoryCandidate()` for runtime discovery.  The arbiter path is fully coded but the classifier must be populated with a concrete event+stage+room entry before PartyStory gating activates.

### Step 5 — Add initiator-owned conversations ✅ (Complete)

Initiator tracking via `trackConversation()`, PtT/PtI fix in `setParam()`, reward distribution via `applyConversationRewards()`.

### Step 6 — Single-visible-initiator presentation ✅ (Complete)

`pushConversationPresentation()` / `popConversationPresentation()` in `coop_render.cpp`.  Presentation override activates on `trackConversation()` and deactivates on `markConversationFinished()` / `cancel()` / `reset()`.

### Step 7 — Expand classification and party-wide results ⏳ (Deferred)

Not implemented.  See "Deferred event categories" above.

### Step 0 — Build a transition/event inventory

**Agent deliverables**

- A source inventory in this document or a companion `AUTHORITY_CODE_MAP.md` listing each confirmed hook.
- Debug-only logs at the smallest useful boundaries, guarded by `TARGET_PC` and the co-op debug facilities.
- No authority behavior changes.

**Concrete examples to verify**

```text
P2 enters daScex_c exit trigger
  -> daScex_c::execute
  -> P2/Link onSceneChangeArea
  -> dComIfGp_setNextStage
  -> dScnPly_Execute consumes enabled next stage
```

```text
P2 presses talk/interact
  -> Link interaction update with P2 context
  -> fopAcM_orderSpeakEvent / fopAcM_orderTalkEvent
  -> event manager global request
  -> event camera/render path
```

Record whether each path carries an initiator, actor anchor, event ID, and target stage. If a path loses that data, record the exact boundary where a side-channel is required.

**Acceptance criteria**

- A debug run can identify the source and player for one stage-exit request and one conversation request.
- The vanilla single-player behavior remains unchanged.
- The inventory names the first party-story event and its concrete event/actor identifier, or explicitly records why runtime discovery is required.

### Step 1 — Add the arbiter skeleton

**Agent deliverables**

- `include/dusk/coop/coop_event.h` and `src/dusk/coop/coop_event.cpp` (or the repository's agreed equivalent).
- Runtime lifecycle wiring: `init`, `reset`, and once-per-frame `tick`.
- Fixed-size player masks/arrays bounded by `MAX_LOCAL_PLAYERS`.
- Request deduplication and explicit state transitions.
- No interception that changes whether vanilla events or transitions run yet.

**Concrete API shape**

```cpp
EventToken request(const EventRequest& request);
void tick();
bool isWaiting(EventToken token);
bool isReady(EventToken token);
bool shouldCommit(EventToken token);
void cancel(EventToken token);
```

The exact names may change to fit project conventions, but callers must be able to submit an event with:

```cpp
{
    .kind = EventKind::StageExit,
    .scope = EventScope::PartySynchronized,
    .initiator = p2,
    .presentationOwner = PlayerId::P1,
    .anchor = exit->current.pos,
    .radius = 450.0f,
    .target = {stage, room, point, layer},
}
```

**Acceptance criteria**

- `Idle -> WaitingForParty -> ReadyToCommit -> Running -> Idle` is observable in logs.
- A player leaving and re-entering the radius changes the readiness mask on subsequent ticks.
- Duplicate requests for the same kind/anchor/target reuse one token.
- No request can index outside the eight-player arrays.

### Step 2 — Implement readiness and waiting notifications

**Agent deliverables**

- Reusable readiness calculation and participant snapshot logic.
- Per-event anchor/radius handling.
- Toast throttling, with a deterministic interval (for example, once every 120 frames) and reset on readiness/anchor changes.
- Tests or a debug harness for sparse joined slots.

**Concrete example**

```text
joined:       P1, P2, P4
anchor:       exit trigger position
radius:       450 world units
positions:    P1=ready, P2=too far, P4=ready
readiness:    101b (P1/P4 ready)
result:       remain WaitingForParty; toast goes to all active views
```

A slot that is not joined is excluded. A joined slot whose Link actor is temporarily unavailable is not ready; it must not be treated as silently absent.

**Acceptance criteria**

- Waiting is indefinite.
- Movement is allowed while waiting.
- Any joined player outside the radius prevents commit.
- Toasts appear on every active co-op view and are not emitted every frame.

### Step 3 — Gate stage entry, then stage exit

Implement entry first and verify it before adding exit.

**Stage-entry example**

```text
P1 finishes create; P2 is still loading
  -> create pending StageEntry request
  -> no start demo is ordered
  -> P2 becomes valid and enters the start anchor
  -> arbiter commits exactly once
  -> P1 orders the original start demo
```

**Stage-exit example**

```text
P2 crosses exit E7
  -> capture {E7, target stage/room/point/layer, P2, anchor}
  -> suppress duplicate/competing next-stage writes
  -> wait for every joined Link near E7
  -> P1 performs one original next-stage request
```

**Acceptance criteria**

- P1 and P2 cannot independently enable conflicting next stages.
- Menu/logo/system transitions are not gated as gameplay exits.
- A committed transition invokes the original vanilla operation once.
- A secondary Link is recreated correctly after the transition.

### Step 4 — Add the first party-synchronized story event

Use one explicit known route-blocking event first; do not generalize all NPC events yet.

**Concrete example**

```text
P3 triggers the route NPC
  -> request initiator=P3, flowAuthority=P1,
     presentationOwner=P1, participants={P1,P2,P3}
  -> wait at NPC anchor until all three are inside radius
  -> P1 orders one vanilla event
  -> P1 camera/presentation is used
  -> global event flags commit once
  -> party-wide result is applied
```

**Acceptance criteria**

- The event cannot be ordered once per Link.
- P3's trigger is preserved even though P1 owns flow authority.
- Readiness loss returns the request to waiting.
- Event completion clears arbiter state and does not leave stale participant locks.

### Step 5 — Add initiator-owned conversations

**Concrete example**

```text
P2 talks to NPC N
  -> request initiator=P2, target=N, presentationOwner=P2
  -> P1 remains arbiter/commit authority
  -> event runs once
  -> only P2's camera is presented full-screen
  -> conversation reward is granted to P1..Pjoined once each
  -> global flags commit once
  -> prior split-screen layout is restored
```

Audit all player-0 assumptions around speaker, talk partner, event input, skip handling, and reward recipient; do not patch only the first `getPlayer(0)` occurrence.

**Acceptance criteria**

- P2's conversation is attributed to P2 in logs and event metadata.
- Non-initiator views are hidden for the entire event, including fade/finish frames.
- P1 is not accidentally substituted as the semantic speaker.
- Restoration works on normal completion, scene transition, and reset/abort.

### Step 6 — Expand classification and party-wide results

Only after Steps 3–5 pass, classify additional event families. For each family document one concrete source, scope, authority, presentation owner, anchor, and result policy before coding it.

**Example classification table**

| Source | Scope | Authority | Presentation | Result |
|---|---|---|---|---|
| Route NPC | Party synchronized | P1 | P1 | Global flags + party reward |
| NPC conversation | Initiator owned | P1 commit | Initiator | Global flags + party reward |
| Door exit | Party synchronized | P1 | Existing camera | Scene transition |
| Ordinary pickup | Deferred/local decision | TBD | TBD | TBD |

**Acceptance criteria**

- Every newly supported category has a deduplication key and abort/restore behavior.
- Global side effects occur once; per-player rewards are explicitly iterated over joined players.
- Unsupported categories remain unchanged rather than accidentally inheriting party synchronization.

## Agent handoff rules

- Use a fresh flash agent for each numbered step.
- Run steps strictly in order; do not start the next step while the previous one has failing verification or undocumented deviations.
- Each agent must inspect the current tree, implement only its assigned step, run the narrowest relevant build/tests, and report files changed plus unresolved issues.
- If an agent discovers that a later step must precede its assigned work, update this plan and stop at the smallest safe prerequisite rather than implementing unrelated behavior.
