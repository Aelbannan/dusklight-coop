/**
 * d_a_alldie.cpp
 * Activates a switch when all enemies are defeated 
*/

#include "d/dolzel_rel.h" // IWYU pragma: keep

#include "d/actor/d_a_alldie.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"

#if TARGET_PC
#include "dusk/coop/coop_enemy.h"
#endif

u8 daAlldie_c::getEventNo() {
    return fopAcM_GetParam(this) >> 0x18;
}

u8 daAlldie_c::getSwbit() {
    return fopAcM_GetParam(this) >> 0x8;
}

int daAlldie_c::actionWait() {
    return 1;
}

int daAlldie_c::actionCheck() {
    const s8 roomNo = fopAcM_GetRoomNo(this);
    if (fopAcM_myRoomSearchEnemy(roomNo) == NULL) {
#if TARGET_PC
        // Co-op (M2, 03-enemies.md §5): on the host the room-clear moment is
        // authoritative — broadcast the per-room EnemyEvent(RoomClear) bit so
        // clients hold their own ALLDIE instead of opening the door early
        // (corpse-linger / dynamic-spawn edges). On a client, hold the
        // ACT_CHECK -> ACT_TIMER transition until that bit arrives for rooms
        // that had synced enemies (their death mirror is authoritative);
        // rooms without synced enemies behave vanilla. Both calls no-op on
        // the other role.
        dusk::coop::enemy::hostRoomCleared(roomNo);
        if (dusk::coop::enemy::clientRoomClearGated(roomNo)) {
            return 1;  // client: wait for the owner's roomClear bit
        }
#endif
        mAction = ACT_TIMER;
        mTimer = 65;
    }

    return 1;
}

int daAlldie_c::actionTimer() {
    if (fopAcM_myRoomSearchEnemy(fopAcM_GetRoomNo(this)) != NULL) {
        mAction = ACT_CHECK;
    } else {
#if TARGET_PC
        // Co-op (M2): a rescan racing the roomClear bit (e.g. the room gained
        // a synced enemy between actionCheck and here) must not let the timer
        // expire early on the client.
        if (dusk::coop::enemy::clientRoomClearGated(fopAcM_GetRoomNo(this))) {
            mAction = ACT_CHECK;
            return 1;
        }
#endif
        if (mTimer > 0) {
            mTimer--;
        } else {
            if (mEventIdx == -1) {
                mAction = ACT_WAIT;
            } else {
                mAction = ACT_ORDER;
            }

            dComIfGs_onSwitch(getSwbit(), fopAcM_GetRoomNo(this));
        }
    }

    return 1;
}

int daAlldie_c::actionOrder() {
    if (eventInfo.checkCommandDemoAccrpt()) {
        mAction = ACT_EVENT;
    } else {
        fopAcM_orderOtherEventId(this, mEventIdx, getEventNo(), -1, 0, 1);
    }

    return 1;
}

int daAlldie_c::actionEvent() {
    if (dComIfGp_evmng_endCheck(mEventIdx)) {
        dComIfGp_getEvent()->reset();

        if (mNextEventIdx != -1) {
            mAction = ACT_NEXT;
            fopAcM_orderOtherEventId(this, mNextEventIdx, mMapToolID, -1, 0, 1);
        } else {
            mAction = ACT_WAIT;
            mMapToolID = -1;
        }
    }

    return 1;
}

int daAlldie_c::actionNext() {
    if (eventInfo.checkCommandDemoAccrpt()) {
        mEventIdx = mNextEventIdx;
        s8 roomNo = fopAcM_GetRoomNo(this);

        mNextEventIdx = -1;
        dStage_MapEvent_dt_c* map_evt = dEvt_control_c::searchMapEventData(mMapToolID, roomNo);

        if (map_evt != NULL) {
            mMapToolID = map_evt->field_0x5;
            mNextEventIdx = dComIfGp_getEventManager().getEventIdx(this, mMapToolID);
        } else {
            mMapToolID = -1;
        }

        mAction = ACT_EVENT;
        actionEvent();
    } else {
        fopAcM_orderOtherEventId(this, mNextEventIdx, mMapToolID, -1, 0, 1);
    }

    return 1;
}

int daAlldie_c::execute() {
    switch (mAction) {
    case ACT_CHECK:
        actionCheck();
        break;
    case ACT_TIMER:
        actionTimer();
        break;
    case ACT_ORDER:
        actionOrder();
        break;
    case ACT_EVENT:
        actionEvent();
        break;
    case ACT_NEXT:
        actionNext();
        break;
    default:
        actionWait();
        break;
    }

    return 1;
}

static int daAlldie_Draw(daAlldie_c*) {
    return 1;
}

static int daAlldie_Execute(daAlldie_c* i_this) {
    i_this->execute();
    return 1;
}

static int daAlldie_IsDelete(daAlldie_c*) {
    return 1;
}

static int daAlldie_Delete(daAlldie_c* i_this) {
    i_this->~daAlldie_c();
    return 1;
}

int daAlldie_c::create() {
    fopAcM_ct(this, daAlldie_c);

    s8 roomNo = fopAcM_GetRoomNo(this);

    if (!dComIfGs_isSwitch(getSwbit(), fopAcM_GetRoomNo(this))) {
        mAction = ACT_CHECK;
    } else {
        mAction = ACT_WAIT;
    }

    shape_angle.z = 0;
    shape_angle.x = 0;
    current.angle.z = 0;
    current.angle.x = 0;

    mEventIdx = dComIfGp_getEventManager().getEventIdx(this, getEventNo());
    mMapToolID = -1;
    mNextEventIdx = -1;

    dStage_MapEvent_dt_c* map_evt = dEvt_control_c::searchMapEventData(getEventNo(), roomNo);
    if (map_evt != NULL) {
        mMapToolID = map_evt->field_0x5;
        mNextEventIdx = dComIfGp_getEventManager().getEventIdx(this, mMapToolID);
    }

    eventInfo.setEventId(mEventIdx);
    eventInfo.setMapToolId(getEventNo());

    return cPhs_COMPLEATE_e;
}

static int daAlldie_Create(fopAc_ac_c* i_this) {
    return static_cast<daAlldie_c*>(i_this)->create();
}

static DUSK_CONST actor_method_class l_daAlldie_Method = {
    (process_method_func)daAlldie_Create,
    (process_method_func)daAlldie_Delete,
    (process_method_func)daAlldie_Execute,
    (process_method_func)daAlldie_IsDelete,
    (process_method_func)daAlldie_Draw,
};

DUSK_PROFILE actor_process_profile_definition DUSK_CONST g_profile_ALLDIE = {
    /* Layer ID     */ fpcLy_CURRENT_e,
    /* List ID      */ 2,
    /* List Prio    */ fpcPi_CURRENT_e,
    /* Proc Name    */ fpcNm_ALLDIE_e,
    /* Proc SubMtd  */ &g_fpcLf_Method.base,
    /* Size         */ sizeof(daAlldie_c),
    /* Size Other   */ 0,
    /* Parameters   */ 0,
    /* Leaf SubMtd  */ &g_fopAc_Method.base,
    /* Draw Prio    */ fpcDwPi_ALLDIE_e,
    /* Actor SubMtd */ &l_daAlldie_Method,
    /* Status       */ fopAcStts_UNK_0x40000_e | fopAcStts_UNK_0x4000_e,
    /* Group        */ fopAc_ACTOR_e,
    /* Cull Type    */ fopAc_CULLBOX_6_e,
};
