/**
 * @file d_a_scene_exit.cpp
 * 
*/

#include "d/dolzel_rel.h" // IWYU pragma: keep

#include "d/actor/d_a_scene_exit.h"
#include "d/d_com_inf_game.h"
#include "d/actor/d_a_player.h"
#include "m_Do/m_Do_mtx.h"

#if TARGET_PC
#include "d/actor/d_a_alink.h"
#include "dusk/coop/coop.h"
#include "dusk/coop/coop_accessors.h"
#include "dusk/coop/coop_alink.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_event.h"
#endif

int daScex_c::checkWork() {
    if (getArg1() == 0xFF || getArg1() == 0 || getArg1() == 3) {
        if (fopAcM_isSwitch(this, getSwNo())) {
            return 0;
        }
    } else if ((getArg1() == 1 || getArg1() == 2 || getArg1() == 4) && getSwNo() != 0xFF) {
        if (!fopAcM_isSwitch(this, getSwNo())) {
            return 0;
        }
    }

    u16 eventBit = getOffEventBit();
    if (eventBit != 0x0FFF && dComIfGs_isEventBit(dSv_event_flag_c::saveBitLabels[eventBit])) {
        return 0;
    }

    eventBit = getOnEventBit();
    if (eventBit != 0x0FFF && !dComIfGs_isEventBit(dSv_event_flag_c::saveBitLabels[eventBit])) {
        return 0;
    }

    return 1;
}

inline int daScex_c::create() {
    fopAcM_ct(this, daScex_c);

    mDoMtx_stack_c::transS(current.pos.x, current.pos.y, current.pos.z);
    mDoMtx_stack_c::YrotM(shape_angle.y);
    mDoMtx_inverse(mDoMtx_stack_c::get(), mMatrix);
    scale.x *= 75.0f;
    scale.z *= 75.0f;
    scale.y *= 150.0f;

    return cPhs_COMPLEATE_e;
}

static int daScex_Create(fopAc_ac_c* i_this) {
    daScex_c* scx = static_cast<daScex_c*>(i_this);
    return scx->create();
}

static int daScex_Execute(daScex_c* i_this) {
    return i_this->execute();
}

int daScex_c::execute() {
    daPy_py_c* player = daPy_getPlayerActorClass();
    cXyz spC;
    bool anyPlayerInZone = false;

    if (checkWork()) {
        mDoMtx_multVec(mMatrix, &player->current.pos, &spC);

        if (spC.y >= 0.0f && spC.y <= scale.y && fabsf(spC.x) <= scale.x && fabsf(spC.z) <= scale.z) {
#if TARGET_PC
            dusk::coop::debug::logInfo(
                "Scex exit triggered: exitID=%d pathID=%d arg1=%d "
                "actor=(%.1f,%.1f,%.1f) "
                "scale=(%.1f,%.1f,%.1f)",
                (int)getArg0(), (int)getPathID(), (int)getArg1(),
                current.pos.x, current.pos.y, current.pos.z,
                scale.x, scale.y, scale.z);
#endif
            anyPlayerInZone = true;

#if TARGET_PC
            // Do not put P1 into vanilla scene-change state while a party
            // barrier is being gathered.  Setting mExitID causes Link's
            // normal update path to enter the exit procedure and prevents
            // movement even though the arbiter has intentionally deferred
            // the transition.  The arbiter captures and commits the exit
            // directly once everyone is ready.
            const bool coopPartyExit = dusk::coop::runtime().joinedPlayerCount > 1;
#else
            const bool coopPartyExit = false;
#endif
            if (!coopPartyExit) {
                switch (getArg1()) {
                case 0xFF:
                case 1:
                    player->onSceneChangeArea(getArg0(), getPathID(), this);
                    break;
                case 2:
                case 0:
                    player->onSceneChangeAreaJump(getArg0(), getPathID(), this);
                    break;
                case 3:
                case 4:
                    player->onSceneChangeAreaJump(getArg0(), getPathID(), this);
                    break;
                }
            }
        }

#if TARGET_PC
        // In co-op, also check every joined player against the exit zone
        // (not just the vanilla slot-0 player).
        if (dusk::coop::runtime().joinedPlayerCount > 1) {
            for (dusk::coop::PlayerId pid = 0; pid < dusk::coop::MAX_LOCAL_PLAYERS; ++pid) {
                if (!dusk::coop::isJoined(pid)) continue;
                fopAc_ac_c* coopPlayer = dusk::coop::getPlayerActor(pid);
                if (coopPlayer == nullptr) continue;
                // Skip the vanilla slot-0 player; already handled above.
                if (pid == 0) continue;

                mDoMtx_multVec(mMatrix, &coopPlayer->current.pos, &spC);
                if (spC.y >= 0.0f && spC.y <= scale.y &&
                    fabsf(spC.x) <= scale.x && fabsf(spC.z) <= scale.z)
                {
                    anyPlayerInZone = true;
                    dusk::coop::debug::logInfo(
                        "Scex: P%u in exit zone exitId=%d",
                        static_cast<unsigned>(pid), (int)getArg0());
                    // Do not set mExitID on secondary Links while waiting:
                    // vanilla treats that as an active exit procedure and
                    // locks their movement.  The central arbiter owns the
                    // deferred transition instead.
                }
            }

            // Submit the StageExit request once if any player is in the zone.
            if (anyPlayerInZone) {
                dusk::coop::event::CapturedExitParams exitParams;
                exitParams.exitId = getArg0();
                exitParams.speed = 0.0f;
                exitParams.mode = 0;
                exitParams.roomNo = fopAcM_GetRoomNo(this);
                exitParams.angle = 0;
                exitParams.param5 = -1;
                exitParams.groundPath = false;
                exitParams.anchor = current.pos;

                dusk::coop::event::EventToken exitToken =
                    dusk::coop::event::deferStageExit(exitParams);
                if (exitToken != dusk::coop::event::INVALID_EVENT_TOKEN) {
                    dusk::coop::debug::logInfo(
                        "Scex: submitted StageExit request token=%u exitId=%d",
                        static_cast<unsigned>(exitToken),
                        (int)getArg0());
                }
            }
        }
#endif
    }

    if (mSceneChangeOK && player->checkSceneChangeAreaStart()) {
        if ((getArg1() == 3 || getArg1() == 4) && field_0x598 == 0) {
            mDoAud_seStart(Z2SE_FORCE_BACK, NULL, 0, 0);
            player->voiceStart(Z2SE_WL_V_FALL_TO_RESTART);
            field_0x598 = 1;
        }

        if (getArg1() == 0xFF || getArg1() == 0 || getArg1() == 3) {
            if (getSwNo() != 0xFF) {
                fopAcM_onSwitch(this, getSwNo());
            }
        }
    }

    return 1;
}

static DUSK_CONST actor_method_class l_daScex_Method = {
    (process_method_func)daScex_Create,
    NULL,
    (process_method_func)daScex_Execute,
};

DUSK_PROFILE actor_process_profile_definition2 DUSK_CONST g_profile_SCENE_EXIT = {
    /* Layer ID     */ fpcLy_CURRENT_e,
    /* List ID      */ 10,
    /* List Prio    */ fpcPi_CURRENT_e,
    /* Proc Name    */ fpcNm_SCENE_EXIT_e,
    /* Proc SubMtd  */ &g_fpcLf_Method.base,
    /* Size         */ sizeof(daScex_c),
    /* Size Other   */ 0,
    /* Parameters   */ 0,
    /* Leaf SubMtd  */ &g_fopAc_Method.base,
    /* Draw Prio    */ fpcDwPi_SCENE_EXIT_e,
    /* Actor SubMtd */ &l_daScex_Method,
    /* Status       */ fopAcStts_UNK_0x40000_e | fopAcStts_NOPAUSE_e,
    /* Group        */ fopAc_UNK_GROUP_5_e,
    /* Cull Type    */ fopAc_CULLBOX_0_e,
    /* Unknown      */ 0                      // field_0x30,
};
