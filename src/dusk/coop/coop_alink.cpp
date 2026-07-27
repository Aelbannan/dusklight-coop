#include "dusk/coop/coop_alink.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_accessors.h"
#include "dusk/coop/coop_combat.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_forms.h"
#include "dusk/coop/coop_input.h"
#include "dusk/coop/coop_player.h"

#if TARGET_PC
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "dolphin/pad.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_base.h"
#include "m_Do/m_Do_controller_pad.h"

#include <array>
#include <cmath>
#include <deque>
#endif

namespace dusk::coop::alink {
namespace {

#if TARGET_PC

struct SpawnState {
    bool active = false;
    fpc_ProcID processId = fpcM_ERROR_PROCESS_ID_e;
    BOOL bgWait = FALSE;
};

std::array<SpawnState, MAX_LOCAL_PLAYERS> g_spawns{};
// Spawn bookkeeping is transient. Keep the finished actor-to-player binding
// separately so ownerOf() cannot fall back to P0 after markSpawnComplete().
std::array<const daAlink_c*, MAX_LOCAL_PLAYERS> g_ownedLinks{};
std::deque<PlayerId> g_pendingOrder{};
constexpr PlayerId kUnowned = 0xFF;

f32 stickMagnitude(f32 x, f32 y) {
    const f32 mag = std::sqrt(x * x + y * y);
    return mag > 1.0f ? 1.0f : mag;
}

PlayerId ownerForProcess(fpc_ProcID pid) {
    if (pid == fpcM_ERROR_PROCESS_ID_e) {
        return kUnowned;
    }
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        if (g_spawns[id].active && g_spawns[id].processId == pid) {
            return id;
        }
    }
    return kUnowned;
}

PlayerId ownerForLink(const daAlink_c* link) {
    if (link == nullptr) {
        return kUnowned;
    }
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        if (g_ownedLinks[id] == link) {
            return id;
        }
    }
    return kUnowned;
}

PlayerId claimNextUnbound(fpc_ProcID pid) {
    while (!g_pendingOrder.empty()) {
        const PlayerId id = g_pendingOrder.front();
        g_pendingOrder.pop_front();
        if (!isValidPlayer(id)) {
            continue;
        }
        auto& slot = g_spawns[id];
        if (!slot.active) {
            continue;
        }
        if (slot.processId != fpcM_ERROR_PROCESS_ID_e && slot.processId != pid) {
            continue;
        }
        slot.processId = pid;
        return id;
    }
    return kUnowned;
}

#endif

}  // namespace

void registerPendingSpawn(PlayerId player) {
#if TARGET_PC
    if (!isValidPlayer(player)) {
        return;
    }
    auto& slot = g_spawns[player];
    slot.active = true;
    slot.processId = fpcM_ERROR_PROCESS_ID_e;
    slot.bgWait = FALSE;
    g_pendingOrder.push_back(player);
    debug::logInfo("Link P%u pending create registered", player);
#else
    (void)player;
#endif
}

void noteSpawnProcess(PlayerId player, u32 processId) {
#if TARGET_PC
    if (!isValidPlayer(player) || !g_spawns[player].active) {
        return;
    }
    const auto pid = static_cast<fpc_ProcID>(processId);
    if (pid == fpcM_ERROR_PROCESS_ID_e) {
        return;
    }
    // create() may already have claimed this process via the FIFO.
    if (g_spawns[player].processId == fpcM_ERROR_PROCESS_ID_e ||
        g_spawns[player].processId == pid) {
        g_spawns[player].processId = pid;
    }
#else
    (void)player;
    (void)processId;
#endif
}

void clearSpawn(PlayerId player) {
#if TARGET_PC
    if (!isValidPlayer(player)) {
        return;
    }
    g_spawns[player] = {};
    for (auto it = g_pendingOrder.begin(); it != g_pendingOrder.end();) {
        if (*it == player) {
            it = g_pendingOrder.erase(it);
        } else {
            ++it;
        }
    }
#else
    (void)player;
#endif
}

void clearPendingSpawn() {
#if TARGET_PC
    g_spawns = {};
    g_ownedLinks = {};
    g_pendingOrder.clear();
#endif
}

void markSpawnComplete(PlayerId player) {
#if TARGET_PC
    clearSpawn(player);
#else
    (void)player;
#endif
}

PlayerId resolveOwner(const daAlink_c* link) {
#if !TARGET_PC
    (void)link;
    return 0;
#else
    if (link == nullptr) {
        return 0;
    }

    if (const PlayerId bound = ownerForLink(link); bound != kUnowned) {
        return bound;
    }

    const fpc_ProcID pid = fopAcM_GetID(link);

    if (const PlayerId bound = ownerForProcess(pid); bound != kUnowned) {
        return bound;
    }

    // First create phase only: bind the next unbound pending join to this process.
    if (const PlayerId claimed = claimNextUnbound(pid); claimed != kUnowned) {
        return claimed;
    }

    // Already registered in the native player slot by a later create phase.
    return forms::playerIdForLink(link);
#endif
}

BOOL& bgWaitFlag(PlayerId player) {
#if TARGET_PC
    if (!isValidPlayer(player)) {
        static BOOL dummy = FALSE;
        return dummy;
    }
    return g_spawns[player].bgWait;
#else
    (void)player;
    static BOOL dummy = FALSE;
    return dummy;
#endif
}

PlayerId ownerOf(const daAlink_c* link) {
#if TARGET_PC
    if (link == nullptr) {
        return 0;
    }

    if (const PlayerId bound = ownerForLink(link); bound != kUnowned) {
        return bound;
    }

    const fpc_ProcID pid = fopAcM_GetID(link);
    if (const PlayerId bound = ownerForProcess(pid); bound != kUnowned) {
        return bound;
    }
    // Never claim pending joins here; ownership lookup must not consume create state.
    return forms::playerIdForLink(link);
#else
    (void)link;
    return 0;
#endif
}

bool isStoryAuthorityLink(const daAlink_c* link) {
    if (link == nullptr) {
        return false;
    }
#if TARGET_PC
    const auto* slot = playerSlot(ownerOf(link));
    return slot != nullptr && slot->transitionAuthority;
#else
    return true;
#endif
}

void onLinkCreated(PlayerId id, daAlink_c* link) {
#if TARGET_PC
    if (!isValidPlayer(id) || link == nullptr) {
        return;
    }
    g_ownedLinks[id] = link;
    setPlayerActor(id, static_cast<fopAc_ac_c*>(link));
    combat::registerPlayerActor(id, static_cast<fopAc_ac_c*>(link));
    if (auto* slot = playerSlot(id)) {
        slot->joined = true;
        slot->enabled = true;
        slot->id = id;
        if (!slot->view.has_value()) {
            slot->view = static_cast<ViewId>(id);
        }
    }
    markSpawnComplete(id);
    debug::logInfo("Link P%u created (proc=%u)", id,
                   static_cast<unsigned>(fopAcM_GetID(link)));
#else
    (void)id;
    (void)link;
#endif
}

void clearLinkOwner(PlayerId id, const daAlink_c* link) {
#if TARGET_PC
    if (isValidPlayer(id) && g_ownedLinks[id] == link) {
        g_ownedLinks[id] = nullptr;
    }
#else
    (void)id;
    (void)link;
#endif
}

bool applyInputSnapshot(daAlink_c* link) {
#if !TARGET_PC
    (void)link;
    return false;
#else
    if (link == nullptr) {
        return false;
    }
    const PlayerId id = ownerOf(link);
    const auto& snap = input::snapshot(id);
    link->mStickValue = stickMagnitude(snap.leftStick.x, snap.leftStick.y);
    // Match JUTGamePad::CStick::update() and the vanilla PAD_1 path.  The
    // previous negation of X mirrored left/right movement.
    link->mStickAngle =
        static_cast<s16>(cM_atan2s(snap.leftStick.x, -snap.leftStick.y) - static_cast<s16>(-0x8000));

    auto mapTrig = [&](u16 padBit, daAlink_c::daAlink_ITEM_BTN btn) {
        if (snap.buttonsPressed & padBit) {
            link->mItemTrigger |= btn;
        }
        if (snap.buttonsHeld & padBit) {
            link->mItemButton |= btn;
        }
    };

    mapTrig(PAD_BUTTON_A, daAlink_c::BTN_A);
    mapTrig(PAD_BUTTON_B, daAlink_c::BTN_B);
    mapTrig(PAD_BUTTON_X, daAlink_c::BTN_X);
    mapTrig(PAD_BUTTON_Y, daAlink_c::BTN_Y);
    mapTrig(PAD_TRIGGER_Z, daAlink_c::BTN_Z);
    mapTrig(PAD_TRIGGER_L, daAlink_c::BTN_L);
    mapTrig(PAD_TRIGGER_R, daAlink_c::BTN_R);

    return true;
#endif
}

// ---------------------------------------------------------------------------
// Location-based dialogue trigger helper
// ---------------------------------------------------------------------------

#if TARGET_PC

PlayerId resolveClosestDialoguePlayer(const DialogueTriggerParams& params) {
    PlayerId best = DIALOGUE_PLAYER_NONE;
    f32 bestDist = 1e30f;

    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (!isJoined(i)) {
            continue;
        }
        fopAc_ac_c* player = getPlayerActor(i);
        if (player == nullptr) {
            continue;
        }

        // Standard XZ radius + Y half-height area check.
        {
            const cXyz delta = player->current.pos - params.center;
            const f32 distXZ = std::sqrt(delta.x * delta.x + delta.z * delta.z);
            if (distXZ >= params.radiusXZ) {
                continue;
            }
            if (delta.y < -params.halfHeightY || delta.y > params.halfHeightY) {
                continue;
            }
        }

        // Apply the facing-angle check if requested.
        if (params.needFacingCheck && params.facingArc > 0 && params.tagActor != nullptr) {
            const s16 angleToPlayer = fopAcM_searchActorAngleY(params.tagActor, player);
            const s16 angleDiff = static_cast<s16>(
                static_cast<s16>(angleToPlayer + 0x7FFF) -
                static_cast<s16>(player->current.angle.y));
            const s16 absDiff = std::abs(static_cast<int>(angleDiff));
            if (absDiff > params.facingArc) {
                continue;
            }
        }

        // Track closest by XZ distance to the trigger center.
        const cXyz delta = player->current.pos - params.center;
        const f32 distXZ = std::sqrt(delta.x * delta.x + delta.z * delta.z);
        if (distXZ < bestDist) {
            bestDist = distXZ;
            best = i;
        }
    }

    return best;
}

#else

PlayerId resolveClosestDialoguePlayer(const DialogueTriggerParams&) {
    return 0;  // P1 only on non-PC builds
}

#endif  // TARGET_PC

}  // namespace dusk::coop::alink
