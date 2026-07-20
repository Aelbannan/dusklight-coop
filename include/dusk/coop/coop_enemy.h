#pragma once

#include "dusk/coop/coop_types.h"

class fopAc_ac_c;

namespace dusk::coop::enemy {

// Gate G — one-fodder whitelist entry (Armos / E_AI first).
struct EnemyAdapter {
    s16 procName = -1;
    const char* name = nullptr;
    bool whitelistClone = false;
    u8 maxClonesPerSource = 1;
    u8 maxClonesPerRoom = 4;
    bool allowProgressionSwitch = false;
    f32 capsuleRadius = 80.0f;
    f32 capsuleHeight = 250.0f;
    f32 spawnOffset = 180.0f;
};

void init();
void reset();
void tick();

bool registerAdapter(const EnemyAdapter& adapter);
const EnemyAdapter* findAdapter(s16 procName);

// Spawn choke: queue an eligible vanilla source (never a clone).
void noteEligibleSource(fopAc_ac_c* source);

fpc_ProcID spawnClone(fopAc_ac_c* source, PlayerId reasonPlayer);

bool isClone(fpc_ProcID id);
bool isClone(fopAc_ac_c* actor);

u32 pendingCloneCount();
u32 liveCloneCount();
u32 pendingWaveCount();
bool roomClearBlocked();  // ALLDIE must wait for pending clones / waves

void onRoomUnload();

}  // namespace dusk::coop::enemy
