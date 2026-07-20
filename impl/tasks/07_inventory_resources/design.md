# Inventory and Resources — Design

## One source of truth

Original `dSv_player_c` remains authority for: permanent item slots, first-acquisition flags, equipment/collectible unlocks, quiver/bomb-bag/wallet capacity upgrades, max health progression, bottle-slot unlock, quest/progression data.

Companion save must not become a second authority for these values.

## PlayerResourceView

```cpp
class PlayerResourceView {
public:
    virtual uint16_t health() const = 0;
    virtual void setHealth(uint16_t value) = 0;
    virtual uint16_t rupees() const = 0;
    virtual void setRupees(uint16_t value) = 0;
    virtual BottleState bottle(uint8_t slot) const = 0;
    virtual void setBottle(uint8_t slot, BottleState value) = 0;
    // ... arrows, bombs, oil, magic, oxygen, items, equipment
};
```

- `OriginalPlayerResourceView` → Player 0 backs onto original save
- `SidecarPlayerResourceView` → Players 1-7 read/write sidecar

## Capacity vs current value

**Global (original save):** max health, wallet capacity, arrow/bomb/oil capacity, bottle-slot unlock mask, lantern capability, magic capability.

**Per-player:** current health/rupees/arrows/bombs/oil/magic/oxygen, bomb type per unlocked bag, bottle contents/quantity, selected items/equipment.

## Health progression

Heart pieces/containers are global max. Current health is individual.

```cpp
void onGlobalMaximumHealthIncreased(uint16_t oldMax, uint16_t newMax) {
    forEachJoinedPlayer([&](PlayerId p) {
        auto view = resources(p);
        view.setMaximumHealth(newMax);
        uint16_t missing = oldMax - std::min(oldMax, view.health());
        view.setHealth(newMax > missing ? newMax - missing : 1);
    });
}
```

## Bottle unlocks — global slots, per-player contents

Derive unlock mask from original save:

```cpp
using BottleUnlockMask = uint8_t;
BottleUnlockMask deriveBottleUnlockMask(const dSv_player_c& originalPlayer);
```

**Unlock flow:**
1. Determine which original bottle slot the vanilla grant would unlock
2. Apply original item callback to Player 0/global save
3. If collector is Player 0: preserve contents
4. If collector is secondary: set Player 0's slot to empty, place contents in collector's sidecar
5. Initialize same slot empty for other secondary players

**Per-player operations:**
```cpp
std::optional<uint8_t> findEmptyBottle(PlayerId player);
bool fillBottle(PlayerId player, uint8_t slot, ItemId contents, uint8_t quantity);
bool consumeBottle(PlayerId player, uint8_t slot);
```

## Item grant refactoring

```cpp
void grantGlobalCapability(ItemId item, PlayerId collector);
void grantInitialPerPlayerResources(ItemId item, PlayerId collector);
```

Default for newly unlocked ammo tools: capability global, each current player gets starter ammo, future players get starter on init, further drops individual.
