# Persistence and Save — Design

## Original save authority

Do not change `dSv_save_c` size or layout. Original save stores Player 0 mutable state, global item/progression unlocks, stage/chest/switch/dungeon/event state.

Companion stores: Players 1-7 mutable resources/loadouts, forms/senses defaults, horse summon/restart state, co-op difficulty/settings, optional downed state.

## Per-slot file identity

```
slot-0-save
slot-0-save.coop
slot-1-save
slot-1-save.coop
```

## Header

```cpp
struct CoopSaveHeader {
    char magic[8];        // "DSKCOOP"
    uint32_t version;
    uint32_t headerSize;
    uint64_t generation;
    uint32_t originalSaveCrc;
    uint32_t payloadSize;
    uint32_t payloadCrc;
    uint8_t originalSlot;
};
```

## Two-file consistency

1. Freeze coherent snapshot
2. Write original save to temp
3. Write companion with CRC/generation to temp
4. Flush both
5. Atomically replace original, then companion

On load: if CRC/generation match, load both. If companion missing/mismatched, load original and reinitialize secondary players from global progression. Never reject valid original because companion is stale. Warn and log recovery.

## No Player 0 duplication

Do not persist second authoritative copy of Player 0 resources, transform status, or horse restart fields in companion. Runtime caches allowed; save serialization writes Player 0 through original save only.

## Reconciliation after load

1. Derive global item/capacity/wallet/bomb-bag/max-health/bottle-unlock from original
2. Validate secondary item selections against global availability
3. Clamp counts to global capacities; clear locked bottle/bomb slots; validate item IDs
4. Initialize missing players
5. Restore health ≤ global max
6. Restore each secondary form if allowed by destination stage
7. Restore or mark summonable horses per stage policy
8. Clear transient actor ownership, grabs, projectiles, target IDs

## Device assignments

Controller/device preferences belong in general settings, not game-save state.

## Save position

Only Player 0 position persisted in original save. Secondary players spawn safely near Player 0 after load.
