/**
 * f_op_camera_mng.cpp
 * Camera Process Manager
 */

#include "f_op/f_op_camera_mng.h"
#include "f_pc/f_pc_manager.h"

static fpc_ProcID l_fopCamM_id[fopCamM_MAX_CAMERAS];

u32 fopCamM_GetParam(camera_class* i_this) {
    return fpcM_GetParam(i_this);
}

void dummy(fpc_ProcID i_procName) {
    fpcM_SearchByID(i_procName);
}

fpc_ProcID fopCamM_Create(int i_cameraIdx, s16 i_procName, void* i_append) {
    if (i_cameraIdx < 0 || i_cameraIdx >= fopCamM_MAX_CAMERAS) {
        return fpcM_ERROR_PROCESS_ID_e;
    }
    l_fopCamM_id[i_cameraIdx] = fpcM_Create(i_procName, NULL, i_append);
    return l_fopCamM_id[i_cameraIdx];
}

fpc_ProcID fopCamM_GetID(int i_cameraIdx) {
    if (i_cameraIdx < 0 || i_cameraIdx >= fopCamM_MAX_CAMERAS) {
        return fpcM_ERROR_PROCESS_ID_e;
    }
    return l_fopCamM_id[i_cameraIdx];
}

void fopCamM_ClearID(int i_cameraIdx) {
    if (i_cameraIdx < 0 || i_cameraIdx >= fopCamM_MAX_CAMERAS) {
        return;
    }
    l_fopCamM_id[i_cameraIdx] = fpcM_ERROR_PROCESS_ID_e;
}

int fopCamM_Delete(int i_cameraIdx) {
    if (i_cameraIdx < 0 || i_cameraIdx >= fopCamM_MAX_CAMERAS) {
        return 0;
    }
    const fpc_ProcID id = l_fopCamM_id[i_cameraIdx];
    if (id == fpcM_ERROR_PROCESS_ID_e) {
        return 0;
    }
    base_process_class* proc = fpcM_SearchByID(id);
    if (proc != NULL) {
        fpcM_Delete(proc);
    }
    l_fopCamM_id[i_cameraIdx] = fpcM_ERROR_PROCESS_ID_e;
    return 1;
}

void fopCamM_Management() {}

void fopCamM_Init() {
    for (int i = 0; i < fopCamM_MAX_CAMERAS; ++i) {
        l_fopCamM_id[i] = fpcM_ERROR_PROCESS_ID_e;
    }
}
