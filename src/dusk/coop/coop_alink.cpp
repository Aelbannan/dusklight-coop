#include "dusk/coop/coop_alink.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_accessors.h"
#include "dusk/coop/coop_combat.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_forms.h"
#include "dusk/coop/coop_input.h"
#include "dusk/coop/coop_player.h"

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "dolphin/pad.h"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_controller_pad.h"

#include <cmath>
#endif

namespace dusk::coop::alink {
namespace {

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC

bool g_hasPending = false;
PlayerId g_pendingOwner = 0;
BOOL g_secondaryBgWait = FALSE;
PlayerId g_creatingOwner = 0;

f32 stickMagnitude(f32 x, f32 y) {
    const f32 mag = std::sqrt(x * x + y * y);
    return mag > 1.0f ? 1.0f : mag;
}

#endif

}  // namespace

void registerPendingSpawn(PlayerId player) {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    if (player == 0 || !isValidPlayer(player)) {
        return;
    }
    g_hasPending = true;
    g_pendingOwner = player;
    g_secondaryBgWait = FALSE;
#else
    (void)player;
#endif
}

bool hasPendingSpawn() {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    return g_hasPending && g_pendingOwner != 0;
#else
    return false;
#endif
}

PlayerId peekPendingOwner() {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    return g_hasPending ? g_pendingOwner : static_cast<PlayerId>(0);
#else
    return 0;
#endif
}

PlayerId consumePendingOwner() {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    if (!g_hasPending) {
        return 0;
    }
    const PlayerId id = g_pendingOwner;
    g_hasPending = false;
    g_pendingOwner = 0;
    return id;
#else
    return 0;
#endif
}

void clearPendingSpawn() {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    g_hasPending = false;
    g_pendingOwner = 0;
    g_secondaryBgWait = FALSE;
#endif
}

BOOL& secondaryBgWaitFlag() {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    return g_secondaryBgWait;
#else
    static BOOL dummy = FALSE;
    return dummy;
#endif
}

void setCreatingOwner(PlayerId id) {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    g_creatingOwner = id;
#else
    (void)id;
#endif
}

PlayerId creatingOwner() {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    return g_creatingOwner;
#else
    return 0;
#endif
}

bool isCreatingSecondary() {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    return g_creatingOwner != 0;
#else
    return false;
#endif
}

bool isSecondaryLink(const daAlink_c* link) {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    if (link == nullptr || !isEnabled()) {
        return false;
    }
    const PlayerId id = forms::playerIdForLink(link);
    return id != 0;
#else
    (void)link;
    return false;
#endif
}

PlayerId ownerOf(const daAlink_c* link) {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    if (link == nullptr) {
        return 0;
    }
    // Prefer creating-owner during create (sidecar may not be set yet).
    if (g_creatingOwner != 0) {
        return g_creatingOwner;
    }
    return forms::playerIdForLink(link);
#else
    (void)link;
    return 0;
#endif
}

void onSecondaryCreated(PlayerId id, daAlink_c* link) {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    if (id == 0 || link == nullptr) {
        return;
    }
    setPlayerActor(id, static_cast<fopAc_ac_c*>(link));
    combat::registerPlayerActor(id, static_cast<fopAc_ac_c*>(link));
    if (auto* slot = playerSlot(id)) {
        slot->joined = true;
        slot->enabled = true;
        slot->id = id;
        slot->actor = static_cast<fopAc_ac_c*>(link);
        if (!slot->view.has_value()) {
            slot->view = static_cast<ViewId>(id);
        }
    }
    debug::logInfo("Secondary Link P%u created (proc=%u)", id,
                   static_cast<unsigned>(fopAcM_GetID(link)));
#else
    (void)id;
    (void)link;
#endif
}

bool applyInputSnapshot(daAlink_c* link) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)link;
    return false;
#else
    if (link == nullptr || !isEnabled()) {
        return false;
    }
    const PlayerId id = ownerOf(link);
    if (id == 0) {
        return false;
    }

    const auto& snap = input::snapshot(id);
    link->mStickValue = stickMagnitude(snap.leftStick.x, snap.leftStick.y);
    // Match mDoCPd_c::getStickAngle3D convention used by setStickData.
    link->mStickAngle =
        static_cast<s16>(cM_atan2s(-snap.leftStick.x, snap.leftStick.y) - static_cast<s16>(-0x8000));

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

}  // namespace dusk::coop::alink
