# Enemy Augmentation — Design

## Strict adapter whitelist

Only duplicate enemies with an explicit adapter.

```cpp
enum class CoopSpawnClass { Never, Fodder, Standard, Elite, Scripted, Boss };

struct EnemySpawnAdapter {
    s16 processName;
    CoopSpawnClass spawnClass;
    uint8_t maxCopiesPerSource;
    uint8_t maxCopiesPerRoom;
    bool (*canDuplicate)(const fopAc_ac_c& source);
    u32 (*sanitizeParameters)(const fopAc_ac_c& source);
    s8 (*stageArgument)(const fopAc_ac_c& source);
    bool (*findSpawnPosition)(const fopAc_ac_c& source, uint32_t ordinal, cXyz* outPosition, csXyz* outAngle);
    void (*onCreated)(fopAc_ac_c& clone, const fopAc_ac_c& source);
};
```

## Exact create wrapper

```cpp
fpc_ProcID spawnAugmentedEnemy(const EnemySpawnAdapter& adapter,
    const fopAc_ac_c& source, const cXyz& position,
    const csXyz& angle, const cXyz& scale)
{
    return fopAcM_create(adapter.processName,
        adapter.sanitizeParameters(source),
        &position, fopAcM_GetRoomNo(&source),
        &angle, &scale, adapter.stageArgument(source));
}
```

## No recursive cloning

Maintain `std::unordered_set<fpc_ProcID> augmentedActorIds`. Room scanner ignores pending and augmented actors.

## Parameter audit

For every supported profile, document: switch bit fields, path fields, event ID fields, parent/formation fields, drop fields, behavior variant fields, room restrictions, unique-child spawning, death-side progression, heap/resource usage. No adapter ships with unknown parameter bits.

## Placement

Reject: invalid ground, insufficient clearance, hazard/void, doorway volume, unloaded subroom, path-constrained enemy without compatible path, source home-area violation, excessive proximity to player spawn. Flying enemies use volume clearance.

## Room budgets

```cpp
struct EncounterSpawnBudget {
    uint16_t maxEnemyActors, maxEnemyProjectiles;
    uint32_t maxEstimatedHeapBytes, maxCollisionObjects;
};
```

Stop augmenting before any budget exceeded.

## ALLDIE patch

```cpp
bool coopRoomHasRemainingEnemies(int roomNo) {
    return fopAcM_myRoomSearchEnemy(roomNo) != nullptr ||
           encounterManager().hasPendingWaveEnemies(roomNo);
}
```

Do not create fake enemy sentinel.

## Progression switches

Clones must never independently set an original unique completion switch. Allowed: sanitize switch to "none", exclude the enemy, move completion to encounter controller, explicitly preserve only nonprogression local behavior.
