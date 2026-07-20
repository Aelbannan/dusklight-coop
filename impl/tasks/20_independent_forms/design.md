# Independent Forms — Design

## Per-player form state

```cpp
bool isWolf(PlayerId player) {
    return runtime().forms[player].current == PlayerForm::Wolf;
}
```

Global progression controls whether transformation is unlocked. Stage policy controls allowed forms:

```cpp
enum class FormRule : uint8_t {
    Either,
    ForceHuman,
    ForceWolf,
    NoTransformation,
};
```

## Transformation constraints

Cannot transform while: mounted, grabbed, downed, in global event, carrying incompatible actor, using incompatible item/action, inside actor-specific forbidden volume. Mounted player must dismount first.

## Compatibility wrappers

Replace no-argument global queries:

```cpp
bool daPy_py_c::checkNowWolf() {
#if TARGET_PC
    if (dusk::coop::enabled())
        return dusk::coop::isCurrentContextPlayerWolf();
#endif
    return originalGlobalWolfCheck();
}
```

Outside player/actor context → returns Player 0 form.

## Required audit artifact

Every call to `daPy_py_c::checkNowWolf`, `dComIfGs_getTransformStatus`, `dComIfGs_setTransformStatus`, `daAlink_getAlinkActorClass`, `dComIfGp_getLinkPlayer` must be classified as: `CURRENT_LINK`, `SPECIFIC_ACTOR`, `VIEW_OWNER`, `EVENT_PARTICIPANT`, `STORY_AUTHORITY`, `GLOBAL_UNLOCK`, or `UNSAFE_UNRESOLVED`. Gate fails while any remains UNSAFE_UNRESOLVED.

## Global transform-save write isolation

```cpp
void setPersistedForm(PlayerId player, PlayerForm form) {
    if (player == 0)
        dComIfGs_setTransformStatus(form == PlayerForm::Wolf);
    else
        companionPlayer(player).form = form;
}
```

## Per-player Wolf Midna

Each `daAlink_c` already allocates Wolf Midna rider models (`mpWlMidnaModel`, mask, hand, hair). These are the gameplay rider. Every wolf Link renders its own Midna rider with independent visibility/field state. Standalone `daMidna_c` remains one canonical story actor, hidden during ordinary riding.

## Per-view senses

```cpp
bool shouldRenderSensesEffect(ViewId view) {
    PlayerId owner = cameraRoute(view).inputOwner;
    return isPlayerWolf(owner) && runtime().forms[owner].sensesActive;
}
```

World actors affected by senses remain globally simulated; special visualization emitted only into views whose owners have senses active.

## Story transformation events

Free transformation does not acquire global event manager. Story transformations remain global events and may force one designated participant, force every player to a form, temporarily hide nonparticipants, restore prior independent forms afterward if allowed. Event adapter explicitly declares policy.
