# Pickups and Item Grants — Design

## Collection rule

Whoever physically collects a consumable or rupee pickup receives it. No reservation, personal visibility, or intended-recipient lock.

## Pickup classes

```cpp
enum class PickupClass {
    GlobalUnlock,
    GlobalBottleUnlock,
    Consumable,
    BottleContents,
    Rupee,
    SharedProgress,
};
```

## Collection dispatch

```cpp
bool tryCollectPickup(PlayerId player, ItemId item) {
    switch (classifyPickup(item)) {
    case PickupClass::GlobalUnlock:
        grantGlobalItem(item); return true;
    case PickupClass::GlobalBottleUnlock:
        return unlockBottleSlot(player, initialBottleContents(item));
    case PickupClass::Consumable:
        return grantConsumable(player, item);
    case PickupClass::BottleContents:
        return fillPlayerBottle(player, item);
    case PickupClass::Rupee:
        return addRupees(player, rupeeValue(item));
    case PickupClass::SharedProgress:
        grantSharedProgress(item); return true;
    }
    return false;
}
```

Only destroy pickup on successful collection.

## Full-resource behavior

Full health cannot consume heart; full arrows cannot consume arrows; full bombs cannot consume bomb refill; no empty bottle cannot consume fairy/potion; full wallet cannot consume rupees. Another player may still collect.

## Simultaneous collision resolution

1. Lowest distance to pickup
2. Earliest registered collision
3. Lowest player ID

## Delayed item grants

Store initiating player explicitly:

```cpp
struct PendingItemGrant {
    bool active = false;
    PlayerId recipient = 0;
    ItemId item = ItemId::None;
    fpc_ProcID sourceActor = fpcM_ERROR_PROCESS_ID_e;
};
```

Required for: chest animations, shop dialogue, item-get cutscenes, NPC gifts, minigame rewards, delayed bottle fills.
