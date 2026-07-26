# Co-op Authority Model

## Status

This document records the planned authority model for story and party-wide events. It is a design specification; it does not imply that the behavior is fully implemented yet.

## Terminology

- **P1** means `PlayerId == 0`, the Link marked `transitionAuthority`.
- **Flow authority** controls the global event/stage machinery.
- **Initiator** is the player who caused an interaction or requested an event.
- **Anchor** is the trigger location associated with the event: a stage-start point, exit, NPC, object, or scripted event location.
- **Joined player** means a player currently joined to the co-op session and expected to participate in the party.

P1 is currently both the engine compatibility anchor and the story flow authority. These are conceptually separate roles even when they resolve to the same player.

## Core principle

> P1 owns global story flow. The player who triggered an interaction owns its meaning.

P1 does not need to be the initiator or semantic subject of every event. The global vanilla event manager remains serialized and is started/advanced through P1, while event metadata preserves the initiating player and relevant participants.

## Authority assignments

| Event or state | Flow authority | Initiator / semantic owner | Presentation | Result |
|---|---|---|---|---|
| Stage entry | P1 | Stage/story-defined | P1 | Party-wide |
| Stage exit | P1 | Player who reached the exit | P1 | Party-wide |
| Automatic/story cutscene | P1 | Story-defined or triggering player | P1 | Party-wide |
| Party-synchronized story event | P1 | Story/world-defined | P1, with all Links participating | Party-wide |
| NPC conversation | P1 scheduler | Player who pressed Talk | Initiator | Party-wide for now |
| Other interaction events | Deferred | Deferred | Deferred | Deferred |

For a P2-triggered story event, P2 remains the recorded trigger, but P1 is still the cutscene focus. For a normal conversation, the initiating player remains the conversational subject and camera owner.

## Party readiness barrier

Stage entry, stage exit, and story cutscenes use a party readiness barrier before the global event or transition is committed.

### Membership

The required set is every currently joined player. Initially there are no local-room or participant-only exclusions.

### Readiness

Players may continue moving while the barrier is pending. A player counts as ready only while they remain sufficiently close to the event's anchor:

- Stage entry: the stage-start/story anchor.
- Stage exit: the exit trigger location.
- Story cutscene: the scripted event anchor.

Readiness is continuously reevaluated. If a player moves too far away, that player is no longer counted and the barrier remains pending until they return.

The anchor should use an event-specific proximity radius rather than a single universal distance. A player also needs a valid co-op Link in the active stage to count as ready.

### Waiting behavior

There is no timeout or automatic force-warp policy at this stage. A pending event waits until every joined player is close enough.

While waiting:

- The party remains in normal movement unless the underlying vanilla event has already entered an unavoidable lock state.
- The requested stage transition/cutscene must not be committed early.
- Repeated requests for the same pending event must not restart or duplicate it.
- If a player leaves the required proximity, the event remains pending.

When the barrier is blocked, notify every active viewport with:

```cpp
dusk::ui::push_toast_to_all_views({
    .type = "warning",
    .title = "Gather Up",
    .content = "Gather up to go to the next room.",
    .duration = std::chrono::seconds(4),
});
```

Notifications should be throttled or emitted on meaningful waiting-state changes rather than every frame.

## Stage entry

P1 is the only Link that starts the stage-entry demo and consumes the global stage-start story state. Secondary Links must not independently replay the entry demo or consume P1's transition state.

The entry demo should not begin until every joined Link exists, is valid in the new stage, and is within the entry anchor's readiness radius. P1 remains the visual focus of the entry presentation.

## Stage exit

Any player may reach an exit and request the party transition. The request records the initiating player and the exit anchor, but P1 commits the actual scene transition.

All joined players must remain near that exit anchor before the transition proceeds. Direct or repeated scene-change attempts from other Links must resolve to the existing pending party transition rather than creating competing transitions.

## Story cutscenes

P1 controls the global cutscene flow and is always the presentation focus for this initial model, even when another player triggered the event.

The event should still retain:

- the triggering player;
- the story/event target;
- the anchor and participant set; and
- the resulting global progression change.

The barrier applies before starting the cutscene. Global progression is committed once, not once per Link.

## Party-synchronized story events

Some story events are not P1-only presentations or initiator-owned interactions. They must happen **with the entire party**. The opening NPC that blocks the party from taking a particular route is an example.

A party-synchronized event has these properties:

- P1 controls the global event flow.
- Every joined Link is a participant.
- The event waits for every joined Link to be near the event anchor.
- The event is ordered and executed once, not independently by every Link.
- Every Link enters the appropriate event/lock state and receives the event's party-wide result.
- The NPC, object, event bit, and other global side effects are processed once.
- P1 remains the initial presentation focus, although all Links are semantically participating.

This must not be implemented as “run the NPC event once per Link.” That would duplicate event ordering, dialogue, flags, and side effects. Instead, the event needs a party participant set, for example:

```text
flow authority = P1
scope          = party-synchronized
participants   = all joined players
anchor         = NPC/event trigger location
focus          = P1
commit         = once, globally
```

The opening route-blocking NPC should therefore not merely react to whichever Link happens to reach it first. The first valid trigger may request the event, but the event should not begin until the complete joined party is ready. Once started, secondary Links must not suppress themselves as “proxy” actors; they need to be represented as participants even if the vanilla event script visually focuses on P1.

## Conversations

Any joined player may initiate a conversation. Conversations use the global event manager, so only one global conversation/event may run at a time, but the event request must preserve the initiating player.

For a conversation:

- The initiator owns dialogue input and conversation semantics.
- The initiator owns camera/presentation focus.
- P1 starts/arbitrates the underlying global event.
- Conversation rewards are granted to every joined player for now.
- Global story flags are set once.

This is distinct from story cutscenes, where P1 remains the presentation focus.

The vanilla `fopAcM_orderSpeakEvent()` path currently assumes player 0, while explicit talk ordering can carry an actor. Any future implementation must preserve the initiator instead of accidentally routing all conversations to P1.

### View-scoped presentation

A specific-player-triggered event may still be global to the event manager and progression system without being presented on every viewport.

For conversations and similar initiator-owned events:

- The event is ordered globally and remains serialized with other global events.
- The initiating player's viewport becomes the sole visible presentation view, normally expanded to the full presentation area.
- All other viewports are hidden while the event is active; they must not remain visible as gameplay cells or show duplicate dialogue.
- If the global event freezes the party, only the initiating player's view is shown during that freeze.
- P1 remains the flow authority, but is not the presentation owner for this event type.
- The prior split-screen/view layout is restored when the event ends or is aborted.
- The event's global side effects and party-wide rewards still happen once according to the normal result policy.

Whether non-initiating players are free to move before the event starts, partially locked, or fully frozen while it runs is a separate simulation-policy decision. It must not change the single-visible-initiator-view rule.

## Results and rewards

"Party-wide" has three meanings that should remain separate:

1. **Global progression:** event bits, dungeon/story completion, and stage state are committed once.
2. **Shared unlocks:** progression items or abilities are synchronized to every joined player.
3. **Per-player rewards:** resources and inventory rewards are granted to every joined player for the initial conversation policy.

Unique physical rewards and unusual event-specific ownership rules are deferred.

## Event concurrency

The initial assumption is that the vanilla event manager permits only one global event or stage transition at a time. Therefore:

- New global requests are queued or rejected while another global event is active.
- Simultaneous requests require deterministic ordering.
- Local gameplay may continue only when it does not use the global event manager.
- Stage transitions and story cutscenes must not race with conversations or other global events.

The exact queueing and tie-break policy remains to be defined.

## Deferred event categories

The following are not yet assigned an authority policy:

- doors;
- treasure and item pickups;
- catch/presentation events;
- warps and portals;
- scripted map-tool events;
- autonomous enemy/object events that are not party-synchronized story events;
- minigames and other specialized event flows.

They should eventually be classified as either:

- initiator-owned but globally serialized;
- P1/story-owned; or
- genuinely local/per-player.

## Current engine implications

The current code already suppresses secondary stage-start demos, but stage changes and event orders also originate from many Link, NPC, object, and enemy paths. A complete implementation will therefore need a central gate/arbiter for global event ordering and scene changes, rather than relying only on P1-specific checks.

P1 is expected to remain present for the initial design. Authority reassignment, player absence, and revive-related behavior are deferred until the revive system is designed.
