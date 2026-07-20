#pragma once

// Thin bridge for diverting one-slot dComIfGp camera/window/player APIs.
// Keep this header free of heavy coop includes — it is pulled from d_com_inf_game.h.

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC

#include "global.h"

class camera_class;
class dDlst_window_c;
class fopAc_ac_c;
class cXyz;

namespace dusk::coop::camera {

// When true, callers must not touch original mCameraInfo[1] / mWindow[1] / mPlayerInfo[1].
bool useSidecarCamera(int cameraIndex);
bool useSidecarWindow(int windowIndex);
bool useSidecarPlayer(int playerIndex);

camera_class* sidecarGetCamera(int cameraIndex);
void sidecarSetCamera(int cameraIndex, camera_class* cam);

int sidecarGetCameraWinID(int cameraIndex);
int sidecarGetCameraPlayer1ID(int cameraIndex);
int sidecarGetCameraPlayer2ID(int cameraIndex);

u32 sidecarGetCameraAttentionStatus(int cameraIndex);
BOOL sidecarCheckCameraAttentionStatus(int cameraIndex, u32 flag);
void sidecarSetCameraAttentionStatus(int cameraIndex, u32 flag);
void sidecarOnCameraAttentionStatus(int cameraIndex, u32 flag);
void sidecarOffCameraAttentionStatus(int cameraIndex, u32 flag);

void sidecarSetCameraInfo(int camIdx, camera_class* p_cam, int winId, int player1Id,
                          int player2Id);

f32 sidecarGetCameraZoomScale(int cameraIndex);
void sidecarSetCameraZoomScale(int cameraIndex, f32 scale);
f32 sidecarGetCameraZoomForcus(int cameraIndex);
void sidecarSetCameraZoomForcus(int cameraIndex, f32 focus);

const char* sidecarGetCameraParamFileName(int cameraIndex);
void sidecarSetCameraParamFileName(int cameraIndex, char* name);

void sidecarSaveCameraPosition(int cameraIndex, cXyz* pos, cXyz* target, f32 fovy, s16 bank);
void sidecarLoadCameraPosition(int cameraIndex, cXyz* pos, cXyz* target, f32* fovy, s16* bank);

dDlst_window_c* sidecarGetWindow(int windowIndex);
void sidecarSetWindow(u8 windowIndex, f32 x, f32 y, f32 width, f32 height, f32 nearZ, f32 farZ,
                      int camID, int mode);

fopAc_ac_c* sidecarGetPlayer(int playerIndex);
void sidecarSetPlayer(int playerIndex, fopAc_ac_c* player);

}  // namespace dusk::coop::camera

#endif  // ENABLE_LOCAL_COOP && TARGET_PC
