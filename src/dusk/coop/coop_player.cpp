#include "dusk/coop/coop_player.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_accessors.h"
#include "dusk/coop/coop_camera.h"
#include "dusk/coop/coop_combat.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_forms.h"
#include "dusk/coop/coop_horses.h"
#include "dusk/coop/coop_input.h"
#include "dusk/coop/coop_render.h"

#include <array>
#include <cmath>
#include <cstring>

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
#include "SSystem/SComponent/c_m3d.h"
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/d_bg_s_acch.h"
#include "d/d_com_inf_game.h"
#include "d/d_drawlist.h"
#include "d/d_kankyo.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_camera_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "f_pc/f_pc_node.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_mtx.h"
#endif

namespace dusk::coop::player {

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC

namespace {

constexpr f32 kSoftSepRadius = 45.0f;
constexpr f32 kSoftSepPush = 0.25f;
constexpr f32 kTetherWarnDist = 800.0f;
constexpr f32 kTetherTeleportDist = 1500.0f;
constexpr f32 kProxyMoveSpeed = 28.0f;
constexpr f32 kProxyEyeHeight = 150.0f;
constexpr f32 kStickDeadzone = 0.15f;
constexpr f32 kSpawnOffset = 80.0f;

struct ProxyMeta {
    fpc_ProcID processId = fpcM_ERROR_PROCESS_ID_e;
    bool pendingRecreate = false;
    bool createRequested = false;
};

std::array<ProxyMeta, MAX_LOCAL_PLAYERS> g_meta{};

}  // namespace

// Process-manager actor — must be outside anonymous namespace so g_profile_COOP_PROXY can size it.
class daCoopProxy_c : public fopAc_ac_c {
public:
    PlayerId owner = 0;
    dBgS_AcchCir mAcchCir;
    dBgS_ObjAcch mAcch;
    u32 mShadowId = 0;

    int create();
    int execute();
    int draw();
    int do_delete();
};

namespace {

static bool isProxyAlive(PlayerId id) {
    if (!isValidPlayer(id) || id == 0) {
        return false;
    }
    fopAc_ac_c* actor = getPlayerActor(id);
    if (actor == nullptr) {
        return false;
    }
    if (g_meta[id].processId != fpcM_ERROR_PROCESS_ID_e) {
        fopAc_ac_c* byId = fopAcM_SearchByID(g_meta[id].processId);
        return byId == actor;
    }
    return fopAcM_GetName(actor) == fpcNm_COOP_PROXY_e;
}

static void clearMeta(PlayerId id, bool keepPending) {
    if (!isValidPlayer(id)) {
        return;
    }
    const bool pending = keepPending && g_meta[id].pendingRecreate;
    g_meta[id] = {};
    g_meta[id].pendingRecreate = pending;
}

static s16 cameraYawForPlayer(PlayerId id) {
    ViewId view = id;
    if (auto* slot = playerSlot(id); slot && slot->view.has_value()) {
        view = *slot->view;
    }
    camera_class* cam = getCameraProcess(view);
    if (cam == nullptr) {
        cam = getCameraProcess(0);
    }
    if (cam == nullptr) {
        return 0;
    }
    return fopCamM_GetAngleY(cam);
}

static void syncAttention(fopAc_ac_c* actor) {
    if (actor == nullptr) {
        return;
    }
    actor->eyePos = actor->current.pos;
    actor->eyePos.y += kProxyEyeHeight;
    actor->attention_info.position = actor->eyePos;
}

static void applyGroundAndWall(daCoopProxy_c* proxy) {
    proxy->old.pos = proxy->current.pos;
    proxy->mAcch.CrrPos(dComIfG_Bgsp());
    if (proxy->mAcch.ChkGroundHit()) {
        proxy->current.pos.y = proxy->mAcch.GetGroundH();
    }
    syncAttention(proxy);
}

static int authorityRoom() {
    fopAc_ac_c* p0 = getPlayerActor(0);
    if (p0 == nullptr) {
        return 0;
    }
    return fopAcM_GetRoomNo(p0);
}

static bool tryPlaceNearAuthority(cXyz* outPos, s16* outYaw) {
    fopAc_ac_c* p0 = getPlayerActor(0);
    if (p0 == nullptr || outPos == nullptr || outYaw == nullptr) {
        return false;
    }

    static const cXyz kOffsets[] = {
        {kSpawnOffset, 0.0f, 0.0f},  {-kSpawnOffset, 0.0f, 0.0f},
        {0.0f, 0.0f, kSpawnOffset},  {0.0f, 0.0f, -kSpawnOffset},
        {kSpawnOffset, 0.0f, kSpawnOffset}, {-kSpawnOffset, 0.0f, -kSpawnOffset},
    };

    const s16 yaw = p0->shape_angle.y;
    for (const cXyz& local : kOffsets) {
        cXyz world = local;
        mDoMtx_stack_c::YrotS(yaw);
        mDoMtx_stack_c::multVec(&world, &world);
        world += p0->current.pos;
        world.y += 50.0f;

        if (!fopAcM_gc_c::gndCheck(&world)) {
            continue;
        }
        const f32 groundY = fopAcM_gc_c::getGroundY();
        if (groundY <= -G_CM3D_F_INF) {
            continue;
        }
        world.y = groundY;
        *outPos = world;
        *outYaw = yaw;
        return true;
    }

    *outPos = p0->current.pos;
    outPos->x += kSpawnOffset;
    *outYaw = yaw;
    return true;
}

}  // namespace

int daCoopProxy_c::create() {
    fopAcM_ct(this, daCoopProxy_c);

    owner = static_cast<PlayerId>(fopAcM_GetParam(this) & 0xFF);
    if (!isValidPlayer(owner) || owner == 0) {
        return cPhs_ERROR_e;
    }

    mAcchCir.SetWall(30.0f, 50.0f);
    mAcch.Set(fopAcM_GetPosition_p(this), fopAcM_GetOldPosition_p(this), this, 1, &mAcchCir,
              fopAcM_GetSpeed_p(this), nullptr, nullptr);
    mAcch.CrrPos(dComIfG_Bgsp());

    gravity = -5.0f;
    maxFallSpeed = -80.0f;
    scale.set(1.0f, 1.0f, 1.0f);
    fopAcM_SetMtx(this, NULL);
    fopAcM_SetMin(this, -40.0f, 0.0f, -40.0f);
    fopAcM_SetMax(this, 40.0f, 180.0f, 40.0f);

    syncAttention(this);
    setPlayerActor(owner, this);
    combat::registerPlayerActor(owner, this);

    if (auto* slot = playerSlot(owner)) {
        slot->actor = this;
        slot->joined = true;
        slot->enabled = true;
        slot->id = owner;
        slot->view = static_cast<ViewId>(owner);
    }

    g_meta[owner].processId = fopAcM_GetID(this);
    g_meta[owner].createRequested = false;
    g_meta[owner].pendingRecreate = false;

    debug::logInfo("Proxy P%u created at (%.1f, %.1f, %.1f)", owner, current.pos.x, current.pos.y,
                   current.pos.z);
    return cPhs_COMPLEATE_e;
}

int daCoopProxy_c::execute() {
    if (!isEnabled() || !isJoined(owner)) {
        return 1;
    }

    // Confirm sidecar registration without touching original one-slot player array.
    if (getPlayerActor(owner) != this) {
        setPlayerActor(owner, this);
    }

    const auto& snap = input::snapshot(owner);
    f32 stickX = snap.leftStick.x;
    f32 stickY = snap.leftStick.y;
    const f32 mag = std::sqrt(stickX * stickX + stickY * stickY);

    if (!snap.connected || mag < kStickDeadzone) {
        speedF = 0.0f;
        speed.set(0.0f, speed.y, 0.0f);
    } else {
        const f32 inv = 1.0f / mag;
        stickX *= inv;
        stickY *= inv;
        const f32 speedScale = std::min(mag, 1.0f) * kProxyMoveSpeed;
        const s16 camYaw = cameraYawForPlayer(owner);
        // Stick Y forward / X right relative to camera.
        const s16 moveYaw = camYaw + cM_atan2s(stickX, stickY);
        shape_angle.y = moveYaw;
        current.angle.y = moveYaw;
        speedF = speedScale;
        speed.x = speedScale * cM_ssin(moveYaw);
        speed.z = speedScale * cM_scos(moveYaw);
    }

    // Gravity + integrate.
    speed.y += gravity;
    if (speed.y < maxFallSpeed) {
        speed.y = maxFallSpeed;
    }
    current.pos.x += speed.x;
    current.pos.y += speed.y;
    current.pos.z += speed.z;

    applyGroundAndWall(this);
    if (mAcch.ChkGroundHit()) {
        speed.y = 0.0f;
    }

    // Gate I PoC: X toggles sidecar form (no changeWolf on proxy); Y toggles senses flag.
    if ((snap.buttonsPressed & PAD_BUTTON_X) != 0) {
        const PlayerForm next =
            forms::isWolf(owner) ? PlayerForm::Human : PlayerForm::Wolf;
        forms::beginTransform(owner, next);
    }
    if ((snap.buttonsPressed & PAD_BUTTON_Y) != 0 && forms::isWolf(owner)) {
        forms::setSenses(owner, !forms::state(owner).sensesActive);
        debug::logInfo("forms: P%u senses -> %s", owner,
                       forms::state(owner).sensesActive ? "on" : "off");
    }

    return 1;
}

int daCoopProxy_c::draw() {
    // PoC visual: draw a second instance of Player 0's Link model at the proxy pose.
    daAlink_c* link = static_cast<daAlink_c*>(dComIfGp_getPlayer(0));
    if (link == nullptr || link->mpLinkModel == nullptr) {
        return 1;
    }

    J3DModel* model = link->mpLinkModel;
    Mtx saved;
    cMtx_copy(model->getBaseTRMtx(), saved);

    mDoMtx_stack_c::transS(current.pos.x, current.pos.y, current.pos.z);
    mDoMtx_stack_c::YrotM(shape_angle.y);
    model->setBaseTRMtx(mDoMtx_stack_c::get());

    g_env_light.settingTevStruct(0, &current.pos, &tevStr);
    g_env_light.setLightTevColorType_MAJI(model, &tevStr);
    mDoExt_modelUpdateDL(model);

    model->setBaseTRMtx(saved);

    cXyz shadowPos = current.pos;
    shadowPos.y += 50.0f;
    mShadowId = dComIfGd_setShadow(mShadowId, 1, model, &shadowPos, 400.0f, 0.0f, current.pos.y,
                                   mAcch.GetGroundH(), mAcch.m_gnd, &tevStr, 0, 1.0f,
                                   dDlst_shadowControl_c::getSimpleTex());
    return 1;
}

int daCoopProxy_c::do_delete() {
    if (isValidPlayer(owner) && getPlayerActor(owner) == this) {
        combat::unregisterPlayerActor(owner);
        setPlayerActor(owner, nullptr);
        if (auto* slot = playerSlot(owner)) {
            slot->actor = nullptr;
        }
    }
    if (isValidPlayer(owner)) {
        g_meta[owner].processId = fpcM_ERROR_PROCESS_ID_e;
        g_meta[owner].createRequested = false;
    }
    return 1;
}

namespace {

static int daCoopProxy_Create(fopAc_ac_c* i_this) {
    return static_cast<daCoopProxy_c*>(i_this)->create();
}

static int daCoopProxy_Delete(daCoopProxy_c* i_this) {
    return i_this->do_delete();
}

static int daCoopProxy_Execute(daCoopProxy_c* i_this) {
    return i_this->execute();
}

static int daCoopProxy_Draw(daCoopProxy_c* i_this) {
    return i_this->draw();
}

static int daCoopProxy_IsDelete(daCoopProxy_c* /*i_this*/) {
    return 1;
}

// File-local method table; exposed via coopProxyMethodClass() for the global profile.
DUSK_CONST actor_method_class l_daCoopProxy_Method = {
    (process_method_func)daCoopProxy_Create,
    (process_method_func)daCoopProxy_Delete,
    (process_method_func)daCoopProxy_Execute,
    (process_method_func)daCoopProxy_IsDelete,
    (process_method_func)daCoopProxy_Draw,
};

}  // namespace

const actor_method_class* coopProxyMethodClass() {
    return &l_daCoopProxy_Method;
}

namespace {

static uint8_t computeJoinedViewSpan() {
    uint8_t span = 1;
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (isJoined(i)) {
            span = static_cast<uint8_t>(i + 1);
        }
    }
    return span;
}

static void syncCamerasForJoined() {
    const uint8_t span = computeJoinedViewSpan();
    if (span < 2) {
        return;
    }
    camera::ensureCameras(span);
    for (PlayerId i = 1; i < span; ++i) {
        if (!isJoined(i)) {
            continue;
        }
        camera::assignTrackedPlayer(static_cast<ViewId>(i), i);
        camera::assignInputOwner(static_cast<ViewId>(i), i);
        camera::assignAttentionOwner(static_cast<ViewId>(i), i);
        if (auto* slot = playerSlot(i)) {
            slot->view = static_cast<ViewId>(i);
        }
    }
    // Keep Gate A multi-view painter in sync with active cameras.
    render::setForcedViewCount(span);
}

static void resolvePendingCreates() {
    for (PlayerId id = 1; id < MAX_LOCAL_PLAYERS; ++id) {
        auto& meta = g_meta[id];
        if (!meta.createRequested || meta.processId == fpcM_ERROR_PROCESS_ID_e) {
            continue;
        }
        fopAc_ac_c* actor = fopAcM_SearchByID(meta.processId);
        if (actor == nullptr) {
            continue;
        }
        setPlayerActor(id, actor);
        combat::registerPlayerActor(id, actor);
        meta.createRequested = false;
    }
}

static void recreatePendingProxies() {
    if (getPlayerActor(0) == nullptr) {
        return;
    }
    for (PlayerId id = 1; id < MAX_LOCAL_PLAYERS; ++id) {
        if (!g_meta[id].pendingRecreate || !isJoined(id)) {
            continue;
        }
        if (isProxyAlive(id) || g_meta[id].createRequested) {
            g_meta[id].pendingRecreate = false;
            continue;
        }
        if (spawnProxyNearAuthority(id)) {
            g_meta[id].pendingRecreate = false;
        }
    }
}

#endif  // ENABLE_LOCAL_COOP && TARGET_PC

}  // namespace

void init() {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    g_meta = {};
#endif
}

void reset() {
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    destroyAllProxies();
    g_meta = {};
#endif
}

void tick() {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    return;
#else
    if (!isEnabled()) {
        return;
    }

    resolvePendingCreates();
    recreatePendingProxies();

    // Soft separation + tether after proxy execute (same frame is fine; next frame settles).
    for (PlayerId id = 1; id < MAX_LOCAL_PLAYERS; ++id) {
        if (!isJoined(id) || !isProxyAlive(id)) {
            continue;
        }
        softSeparate(0, id);
        tetherTeleportIfNeeded(id, 0);
        for (PlayerId other = id + 1; other < MAX_LOCAL_PLAYERS; ++other) {
            if (isJoined(other) && isProxyAlive(other)) {
                softSeparate(id, other);
            }
        }
    }
#endif
}

bool spawnProxy(PlayerId id, const cXyz& pos, s16 yaw) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)id;
    (void)pos;
    (void)yaw;
    return false;
#else
    if (!isEnabled() || id == 0 || !isValidPlayer(id)) {
        return false;
    }
    if (isProxyAlive(id) || g_meta[id].createRequested) {
        return true;
    }

    // Never write secondary players into the original one-slot player array.
    if (auto* slot = playerSlot(id)) {
        slot->joined = true;
        slot->enabled = true;
        slot->id = id;
        if (!slot->view.has_value()) {
            slot->view = static_cast<ViewId>(id);
        }
    }

    csXyz angle(0, yaw, 0);
    const int roomNo = authorityRoom();

    // Layer must be the play scene so the proxy survives room actor sweeps correctly.
    layer_class* savedLayer = fpcLy_CurrentLayer();
    base_process_class* playScene = fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
    if (playScene != nullptr) {
        fpcLy_SetCurrentLayer(&reinterpret_cast<process_node_class*>(playScene)->layer);
    }

    const fpc_ProcID pid =
        fopAcM_create(fpcNm_COOP_PROXY_e, 0xFFFF, static_cast<u32>(id), &pos, roomNo, &angle,
                      nullptr, -1, nullptr);

    if (playScene != nullptr) {
        fpcLy_SetCurrentLayer(savedLayer);
    }

    if (pid == fpcM_ERROR_PROCESS_ID_e) {
        debug::logError("spawnProxy: fopAcM_create failed for P%u", id);
        return false;
    }

    g_meta[id].processId = pid;
    g_meta[id].createRequested = true;
    g_meta[id].pendingRecreate = false;

    // Resolve immediately when the scheduler already completed create.
    if (fopAc_ac_c* actor = fopAcM_SearchByID(pid)) {
        setPlayerActor(id, actor);
        g_meta[id].createRequested = false;
        combat::registerPlayerActor(id, actor);
    }

    syncCamerasForJoined();
    debug::logInfo("Proxy P%u spawn requested (pid=%u)", id, static_cast<unsigned>(pid));
    return true;
#endif
}

bool spawnProxyNearAuthority(PlayerId id) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)id;
    return false;
#else
    cXyz pos;
    s16 yaw = 0;
    if (!tryPlaceNearAuthority(&pos, &yaw)) {
        debug::logWarn("spawnProxyNearAuthority: no authority player yet for P%u", id);
        if (auto* slot = playerSlot(id)) {
            slot->joined = true;
            slot->enabled = true;
            slot->id = id;
        }
        g_meta[id].pendingRecreate = true;
        return false;
    }
    return spawnProxy(id, pos, yaw);
#endif
}

void destroyProxy(PlayerId id) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)id;
    return;
#else
    if (id == 0 || !isValidPlayer(id)) {
        return;
    }

    const fpc_ProcID pid = g_meta[id].processId;
    fopAc_ac_c* actor = getPlayerActor(id);
    if (actor == nullptr && pid != fpcM_ERROR_PROCESS_ID_e) {
        actor = fopAcM_SearchByID(pid);
    }

    if (actor != nullptr) {
        // Clear registration first so delete callback does not double-clear oddly.
        setPlayerActor(id, nullptr);
        fopAcM_delete(actor);
    } else {
        setPlayerActor(id, nullptr);
    }

    if (auto* slot = playerSlot(id)) {
        slot->actor = nullptr;
        slot->joined = false;
        slot->enabled = false;
        slot->view.reset();
    }
    clearMeta(id, /*keepPending=*/false);
#endif
}

void destroyAllProxies() {
    for (PlayerId i = 1; i < MAX_LOCAL_PLAYERS; ++i) {
        destroyProxy(i);
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
        g_meta[i].pendingRecreate = false;
#endif
    }
}

bool softSeparate(PlayerId a, PlayerId b) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)a;
    (void)b;
    return false;
#else
    if (!isEnabled() || a == b || !isValidPlayer(a) || !isValidPlayer(b)) {
        return false;
    }
    fopAc_ac_c* actorA = getPlayerActor(a);
    fopAc_ac_c* actorB = getPlayerActor(b);
    if (actorA == nullptr || actorB == nullptr) {
        return false;
    }

    cXyz delta = actorB->current.pos - actorA->current.pos;
    delta.y = 0.0f;
    const f32 distance = delta.abs();
    if (distance <= 0.001f || distance >= kSoftSepRadius) {
        return false;
    }

    if (!delta.normalizeRS()) {
        return false;
    }
    const f32 push = (kSoftSepRadius - distance) * kSoftSepPush;
    // Authority (player 0) is immovable; secondary proxies take the full push.
    if (a == 0) {
        actorB->current.pos += delta * (push * 2.0f);
        syncAttention(actorB);
    } else if (b == 0) {
        actorA->current.pos -= delta * (push * 2.0f);
        syncAttention(actorA);
    } else {
        actorA->current.pos -= delta * push;
        actorB->current.pos += delta * push;
        syncAttention(actorA);
        syncAttention(actorB);
    }
    return true;
#endif
}

bool tetherTeleportIfNeeded(PlayerId follower, PlayerId authority) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)follower;
    (void)authority;
    return false;
#else
    if (!isEnabled() || follower == 0 || authority != 0) {
        return false;
    }
    fopAc_ac_c* proxy = getPlayerActor(follower);
    fopAc_ac_c* auth = getPlayerActor(authority);
    if (proxy == nullptr || auth == nullptr) {
        return false;
    }

    cXyz delta = proxy->current.pos - auth->current.pos;
    const f32 distance = delta.abs();
    const bool roomMismatch = fopAcM_GetRoomNo(proxy) != fopAcM_GetRoomNo(auth);

    if (distance > kTetherWarnDist && distance <= kTetherTeleportDist && !roomMismatch) {
        // Soft pull toward authority.
        delta.y = 0.0f;
        if (delta.normalizeRS()) {
            proxy->current.pos -= delta * 8.0f;
            syncAttention(proxy);
        }
        return false;
    }

    if (distance <= kTetherTeleportDist && !roomMismatch) {
        return false;
    }

    cXyz dest;
    s16 yaw = 0;
    if (!tryPlaceNearAuthority(&dest, &yaw)) {
        return false;
    }
    proxy->current.pos = dest;
    proxy->old.pos = dest;
    proxy->home.pos = dest;
    proxy->shape_angle.y = yaw;
    proxy->current.angle.y = yaw;
    proxy->speed.setall(0.0f);
    proxy->speedF = 0.0f;
    syncAttention(proxy);
    debug::logInfo("Proxy P%u tether-teleported (dist=%.1f roomMismatch=%d)", follower, distance,
                   roomMismatch ? 1 : 0);
    return true;
#endif
}

void onRoomUnload() {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    return;
#else
    // Destroy bodies but remember which joined players need a recreate after the next room loads.
    for (PlayerId id = 1; id < MAX_LOCAL_PLAYERS; ++id) {
        const bool wasJoined = isJoined(id) || isProxyAlive(id) || g_meta[id].pendingRecreate;
        if (!wasJoined) {
            continue;
        }

        const fpc_ProcID pid = g_meta[id].processId;
        fopAc_ac_c* actor = getPlayerActor(id);
        if (actor == nullptr && pid != fpcM_ERROR_PROCESS_ID_e) {
            actor = fopAcM_SearchByID(pid);
        }
        if (actor != nullptr) {
            setPlayerActor(id, nullptr);
            fopAcM_delete(actor);
        } else {
            setPlayerActor(id, nullptr);
        }
        if (auto* slot = playerSlot(id)) {
            slot->actor = nullptr;
            // Keep joined so Press-Start slot + input identity survive the transition.
            slot->joined = true;
            slot->enabled = true;
        }
        g_meta[id].processId = fpcM_ERROR_PROCESS_ID_e;
        g_meta[id].createRequested = false;
        g_meta[id].pendingRecreate = true;
    }
#endif
}

bool hasProxy(PlayerId id) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)id;
    return false;
#else
    return isProxyAlive(id) || (isValidPlayer(id) && g_meta[id].pendingRecreate);
#endif
}

fopAc_ac_c* getProxyActor(PlayerId id) {
    if (id == 0) {
        return nullptr;
    }
    return getPlayerActor(id);
}

bool onPlayerJoined(PlayerId id) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)id;
    return false;
#else
    if (!isValidPlayer(id) || id == 0) {
        return false;
    }
    if (!isEnabled()) {
        setEnabled(true);
    }
    const bool spawned = spawnProxyNearAuthority(id);
    syncCamerasForJoined();
    // Gate J: request an owned secondary horse near the new player (may no-op if stage
    // story bits reject horse create — still marks summoned for later retry).
    dusk::coop::horses::spawnOwnedHorseNearPlayer(id);
    return spawned || g_meta[id].pendingRecreate;
#endif
}

}  // namespace dusk::coop::player

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC

// Must be a global symbol — referenced by g_fpcPfLst_ProfileList.
DUSK_PROFILE actor_process_profile_definition DUSK_CONST g_profile_COOP_PROXY = {
    fpcLy_CURRENT_e,
    5,
    fpcPi_CURRENT_e,
    fpcNm_COOP_PROXY_e,
    &g_fpcLf_Method.base,
    sizeof(dusk::coop::player::daCoopProxy_c),
    0,
    0,
    &g_fopAc_Method.base,
    fpcDwPi_ALINK_e,
    // l_daCoopProxy_Method lives in dusk::coop::player's anonymous namespace — not linkable here.
    // Use a file-scope trampoline defined next to the class methods via a getter.
    dusk::coop::player::coopProxyMethodClass(),
    fopAcStts_UNK_0x40000_e | fopAcStts_CULL_e,
    fopAc_ACTOR_e,
    fopAc_CULLBOX_0_e,
};

#endif  // ENABLE_LOCAL_COOP && TARGET_PC
