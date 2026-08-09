#include "dusk/coop/coop_enemy.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_entity_logic.h"

#include "f_op/f_op_actor_mng.h"

#include <aurora/lib/logging.hpp>

#include <cstring>
#include <map>
#include <unordered_map>

namespace dusk::coop::enemy {

namespace {

aurora::Module EnemyLog("dusk::coop::enemy");

// ---------------------------------------------------------------------------
// Registry — entityId <-> actor, per stage. Survives as the M5.2 ghost
// sender's id source (05-ghosts.md §4.1); everything M2/M3/M4 hung on it
// (adapters, snapshots, death polls, freeze/apply, drops, room-clear) is
// shredded. g_entries stays a NODE-BASED std::map so entry addresses are
// stable across inserts (the old M2 synth-collider pointer discipline (deleted in M5.1)); the
// M5.2 sender stores {procName, isDead} on the entry, never derefs the actor
// after `gone` (M4.6 UAF fix carries into the ghost table, 05-ghosts.md §5).
// ---------------------------------------------------------------------------

struct EnemyEntry {
    u16 enemyId = kInvalidEnemyId;
    fpc_ProcID pid = fpcM_ERROR_PROCESS_ID_e;
    fopAc_ac_c* actor = nullptr;
    s8 roomNo = -1;
    EnemyEntry() = default;
    EnemyEntry(const EnemyEntry&) = delete;
    EnemyEntry& operator=(const EnemyEntry&) = delete;
    EnemyEntry(EnemyEntry&&) = default;
    EnemyEntry& operator=(EnemyEntry&&) = default;
};

std::map<u16, EnemyEntry> g_entries;
std::unordered_map<fpc_ProcID, u16> g_pidToEnemyId;
u16 g_dynamicIdCounter = 0;  // per-owner low counter (owner-major id)

// Per-stage reset keys on (stage, room), not room number alone (capstone
// MINOR D — carried over from M4.6; the ghost sender's room-change scan
// will reuse the same tracking in M5.2).
s8 g_lastRoom = -2;
char g_lastStage[net::kMaxStageNameLength] = {};
u32 g_frameCount = 0;

EnemyEntry* FindEntryForActor(const fopAc_ac_c* actor) {
    if (actor == nullptr) {
        return nullptr;
    }
    const auto it = g_pidToEnemyId.find(fopAcM_GetID(const_cast<fopAc_ac_c*>(actor)));
    if (it == g_pidToEnemyId.end()) {
        return nullptr;
    }
    const auto eit = g_entries.find(it->second);
    return eit != g_entries.end() ? &eit->second : nullptr;
}

void EraseEntry(u16 enemyId) {
    const auto it = g_entries.find(enemyId);
    if (it != g_entries.end()) {
        g_pidToEnemyId.erase(it->second.pid);
        g_entries.erase(it);
    }
}

void RegisterActor(fopAc_ac_c* actor, u16 enemyId) {
    if (actor == nullptr || enemyId == kInvalidEnemyId) {
        return;
    }
    if (g_entries.count(enemyId) != 0 || g_pidToEnemyId.count(fopAcM_GetID(actor)) != 0) {
        return;
    }
    auto [it, inserted] = g_entries.try_emplace(enemyId);
    if (!inserted) {
        return;
    }
    EnemyEntry& e = it->second;
    e.enemyId = enemyId;
    e.pid = fopAcM_GetID(actor);
    e.actor = actor;
    e.roomNo = fopAcM_GetRoomNo(actor);
    g_pidToEnemyId.emplace(e.pid, enemyId);
}

void ClearAll() {
    g_entries.clear();
    g_pidToEnemyId.clear();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public: registry (dormant until M5.2 wires the ghost sender)
// ---------------------------------------------------------------------------

u16 entityIdForActor(const fopAc_ac_c* actor) {
    if (actor == nullptr) {
        return kInvalidEnemyId;
    }
    const auto it = g_pidToEnemyId.find(fopAcM_GetID(const_cast<fopAc_ac_c*>(actor)));
    if (it != g_pidToEnemyId.end()) {
        return it->second;
    }
    return kInvalidEnemyId;
}

fopAc_ac_c* actorForEntityId(u16 enemyId) {
    const auto it = g_entries.find(enemyId);
    if (it == g_entries.end()) {
        return nullptr;
    }
    return it->second.actor;
}

bool isRegistered(const fopAc_ac_c* actor) {
    return FindEntryForActor(actor) != nullptr;
}

u16 registerDynamicEnemy(fopAc_ac_c* actor) {
    if (actor == nullptr || isRegistered(actor)) {
        return kInvalidEnemyId;
    }
    // Owner-assigned monotonic counter, OWNER-MAJOR (coop_entity_logic.h):
    // the top bit keeps dynamic ids out of the stage-key space, the upper
    // nibble carries the owning PlayerId. DORMANT in M5.1 — the M5.2 ghost
    // sender activates it for script-spawned enemies and projectiles
    // (05-ghosts.md §4.1/§4.4).
    g_dynamicIdCounter = (g_dynamicIdCounter + 1) & kDynamicCounterMask;
    const u16 enemyId = DynamicEntityId(dusk::coop::selfId(), g_dynamicIdCounter);
    RegisterActor(actor, enemyId);
    return enemyId;
}

// ---------------------------------------------------------------------------
// Public: per-frame lifecycle
// ---------------------------------------------------------------------------

void onGameFrame() {
    ++g_frameCount;

    // Net-off guarantee (d-m8): with no live session the stub touches
    // nothing — zero registry entries, zero ghost sends. Whatever the M2
    // host scan left in the tables is cleared once, then we stay inert.
    if (!dusk::coop::sessionActive()) {
        if (!g_entries.empty()) {
            ClearAll();
        }
        return;
    }

    // Room-change detection -> per-stage state reset. Keyed on (stage, room):
    // room numbers are not unique across stages (spring / house interiors).
    // The M2 room scan, death poll and snapshot senders are GONE; M5.2 puts
    // the ghost sender's immediate-scan + registration back on this seam.
    const s8 roomNow = dusk::coop::localRoomNo();
    const char* stageNow = dusk::coop::localStageName();
    const bool stageChanged = std::strcmp(stageNow, g_lastStage) != 0;
    if (stageChanged || roomNow != g_lastRoom) {
        if (g_lastRoom != -2) {
            EnemyLog.info(
                "enemy: room change {} -> {} (stage '{}' -> '{}'); resetting per-stage state",
                static_cast<s32>(g_lastRoom), static_cast<s32>(roomNow), g_lastStage, stageNow);
            ClearAll();
        }
        std::snprintf(g_lastStage, sizeof(g_lastStage), "%s", stageNow);
        g_lastRoom = roomNow;
    }
}

void shutdown() {
    ClearAll();
    g_lastRoom = -2;
    g_lastStage[0] = '\0';
}

}  // namespace dusk::coop::enemy
