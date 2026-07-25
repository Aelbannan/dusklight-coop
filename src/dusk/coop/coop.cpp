#include "dusk/coop/coop.h"

#include "dusk/coop/coop_accessors.h"
#include "dusk/coop/coop_attention.h"
#include "dusk/coop/coop_bottles.h"
#include "dusk/coop/coop_camera.h"
#include "dusk/coop/coop_combat.h"
#include "dusk/coop/coop_context.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_difficulty.h"
#include "dusk/coop/coop_drops.h"
#include "m_Do/m_Do_lib.h"
#include "dusk/coop/coop_enemy.h"
#include "dusk/coop/coop_forms.h"
#include "dusk/coop/coop_gate_h_selfcheck.h"
#include "dusk/coop/coop_horses.h"
#include "dusk/coop/coop_hud.h"
#include "dusk/coop/coop_input.h"
#include "dusk/coop/coop_inventory.h"
#include "dusk/coop/coop_player.h"
#include "dusk/coop/coop_render.h"
#include "dusk/coop/coop_save.h"

namespace dusk::coop {
namespace {

Runtime g_runtime{};

void initPlayer0() {
    auto& p0 = g_runtime.players[0];
    p0.id = 0;
    p0.joined = true;
    p0.enabled = true;
    p0.transitionAuthority = true;
    p0.view = 0;
    p0.legacyPadPort = 0;
    g_runtime.joinedPlayerCount = 1;
    g_runtime.activeViewCount = 1;
    g_runtime.cameras[0].view = 0;
    g_runtime.cameras[0].inputOwner = 0;
    g_runtime.cameras[0].trackedPlayers = {0};
    g_runtime.horses[0].owner = 0;
}

}  // namespace

Runtime& runtime() { return g_runtime; }


void init() {
#if !TARGET_PC
    g_runtime = {};
    return;
#else
    g_runtime = {};
    initPlayer0();
    mDoLib_clipper::setCullingDisabled(true);
    render::init();
    camera::init();
    input::init();
    player::init();
    inventory::init();
    bottles::init();
    hud::init();
    save::init();
    combat::init();
    enemy::init();
    difficulty::init();
    drops::init();
    forms::init();
    horses::init();
    attention::init();
    debug::init();
    gate_h::runSelfChecks();
#endif
}

void reset() {
    const bool wasEnabled = g_runtime.enabled;
    g_runtime = {};
    initPlayer0();
    g_runtime.enabled = wasEnabled;
    render::reset();
    camera::reset();
    input::reset();
    player::reset();
    inventory::reset();
    bottles::reset();
    hud::reset();
    save::reset();
    combat::reset();
    enemy::reset();
    difficulty::reset();
    drops::reset();
    forms::reset();
    horses::reset();
    debug::reset();
}

void tick() {
#if !TARGET_PC
    return;
#else
    // Input always ticks when compiled in so Press-Start can enable co-op.
    input::tick();
    debug::drawOverlay();
    camera::tick();
    player::tick();
    forms::tick();
    horses::tick();
    attention::tick();
    enemy::tick();
    hud::tick();
    combat::endFrame();
    combat::beginFrame();
    assertContextStackEmpty();
#endif
}

void onRoomUnload() {
    player::onRoomUnload();
    horses::onRoomUnload();
    enemy::onRoomUnload();
    camera::destroySecondaryCameras();
    assertContextStackEmpty();
}

void onCoopDisable() {
    player::destroyNonAuthorityLinks();
    horses::destroyAllHorses();
    camera::destroySecondaryCameras();
    enemy::onRoomUnload();
    render::reset();
    g_runtime.activePlayer = 0;
    g_runtime.activeView = 0;
    g_runtime.activeEnemyTarget = nullptr;
    g_runtime.joinedPlayerCount = 1;
    g_runtime.activeViewCount = 1;
    assertContextStackEmpty();
}

PlayerId activePlayer() { return g_runtime.activePlayer; }
ViewId activeView() { return g_runtime.activeView; }

bool isValidPlayer(PlayerId id) { return id < MAX_LOCAL_PLAYERS; }
bool isValidView(ViewId id) { return id < MAX_LOCAL_VIEWS; }

bool isJoined(PlayerId id) {
    return isValidPlayer(id) && g_runtime.players[id].joined;
}

PlayerSlot* playerSlot(PlayerId id) {
    return isValidPlayer(id) ? &g_runtime.players[id] : nullptr;
}

PlayerRuntime* playerRuntime(PlayerId id) {
    return isValidPlayer(id) ? &g_runtime.playerRuntime[id] : nullptr;
}

HorseSlot* horseSlot(PlayerId id) {
    return isValidPlayer(id) ? &g_runtime.horses[id] : nullptr;
}

CameraRoute* cameraRoute(ViewId id) {
    return isValidView(id) ? &g_runtime.cameras[id] : nullptr;
}

}  // namespace dusk::coop
