#pragma once

#include "dusk/coop/coop_context.h"
#include "dusk/coop/coop_types.h"

#include <optional>

class daAlink_c;

namespace dusk::coop::horses {

enum class HorseQueryKind : uint8_t {
    OwnedHorse = 0,
    MountedHorse,
    ActualCollidingActor,
    EventHorse,
    Player0Compatibility,
    UnsafeUnresolved,
};

void init();
void reset();
void tick();
void onRoomUnload();

HorseSlot& slot(PlayerId id);

// Gate J — spawn / destroy
bool spawnOwnedHorse(PlayerId id, const cXyz& pos, s16 yaw);
bool spawnOwnedHorseNearPlayer(PlayerId id);
void destroyOwnedHorse(PlayerId id);
void destroyAllHorses();

// Context-aware resolve
daHorse_c* resolveForContext();
daHorse_c* resolveOwned(PlayerId id);
daHorse_c* resolveMounted(PlayerId id);
daHorse_c* horseForPlayer(PlayerId id);
daHorse_c* mountedHorseForLink(const daAlink_c* link);

bool setMounted(PlayerId id, bool mounted);
u8 liveHorseCount();

PlayerId ownerOf(const daHorse_c* horse);

// Pending-create token consumed by daHorse_c::create.
void beginPendingCreate(PlayerId id);
PlayerId peekPendingCreateOwner();
bool hasPendingCreate();
void onCreateSuccess(PlayerId id, daHorse_c* horse, fpc_ProcID pid);
void onCreateFailed(PlayerId id);
void onHorseDestroyed(daHorse_c* horse);

// Indexed restart state.
void setRestart(PlayerId id, const char* stage, const cXyz& pos, s16 yaw, s8 room);

// Horse execution owner context for AI/collision that must not rely on ambient context.
class ScopedHorseOwnerContext {
public:
    explicit ScopedHorseOwnerContext(const daHorse_c& horse);
    ~ScopedHorseOwnerContext() = default;

    ScopedHorseOwnerContext(const ScopedHorseOwnerContext&) = delete;
    ScopedHorseOwnerContext& operator=(const ScopedHorseOwnerContext&) = delete;

private:
    std::optional<ScopedContext> ctx_;
};

struct HorseSearchContext {
    daHorse_c* horse = nullptr;
    PlayerId owner = 0;
    f32 maximumDistance = 0.0f;
};

const char* queryKindName(HorseQueryKind kind);

}  // namespace dusk::coop::horses
