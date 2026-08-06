#pragma once

/**
 * \file coop_entity_logic.h
 * M4 — enemy entity-id stability across ownership transfer
 * (03-enemies.md §3.1, implementation-plan M4 task 14).
 *
 * Stage-placed enemies key by `(roomNo, setID)`: a pure function of stage
 * data, identical on EVERY machine, so when the room owner leaves and the
 * next player takes over, the new owner registers the same ids and clients'
 * id-keyed maps (g_entries/g_received) keep working with zero renumbering —
 * the transfer is a map transfer, not a renumber (the plan's requirement).
 *
 * Dynamic spawns get an OWNER-MAJOR counter: the top bit separates dynamic
 * ids from the stage-key space, the next bits carry the owner PlayerId, and
 * the low counter wraps per owner. After a takeover the new owner re-sims
 * its own story (dead enemies may resurrect — accepted) and its dynamic ids
 * can never collide with the previous owner's (the old owner's dynamic
 * entities do not exist on the new owner's sim; clients learn new dynamic
 * ids via EnemyEvent(spawn), M5).
 */

#include "dolphin/types.h"

namespace dusk::coop::enemy {

constexpr u16 kInvalidEnemyId = 0xFFFF;
/// Dynamic ids live above the stage-key space (stage keys use the low byte
/// for setID and never set bit 15: setID < 0x100).
constexpr u16 kDynamicIdBase = 0x8000;
constexpr u16 kDynamicCounterMask = 0x0FFF;

/// Stage-placed id: `(roomNo << 8) | setID`. Room numbers in TP stages are
/// < 64 and stage-placed setIDs are < 0x100, so the packing is unique per
/// stage and identical on every machine with the same stage.
inline u16 StageEntityId(int roomNo, int setID) {
    if (setID == 0xFFFF || roomNo < 0 || setID >= 0x100) {
        return kInvalidEnemyId;
    }
    return static_cast<u16>((static_cast<u16>(roomNo & 0xFF) << 8) | (setID & 0xFF));
}

/// Dynamic-spawn id: top bit + owner PlayerId in the upper nibble + the
/// owner's own monotonic counter. Two different owners' dynamic id spaces
/// are disjoint by construction, so an ownership transfer can never produce
/// a phantom/colliding id on the clients' maps.
inline u16 DynamicEntityId(u8 ownerId, u16 counter) {
    return static_cast<u16>(kDynamicIdBase | ((ownerId & 0x7) << 12) |
                            (counter & kDynamicCounterMask));
}

}  // namespace dusk::coop::enemy
