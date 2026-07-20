#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "SSystem/SComponent/c_xyz.h"
#include "f_pc/f_pc_manager.h"

class daAlink_c;
class daHorse_c;
class fopAc_ac_c;
struct camera_class;
class dDlst_window_c;

namespace dusk::coop {

using PlayerId = uint8_t;
using ViewId = uint8_t;

constexpr size_t MAX_LOCAL_PLAYERS = 8;
constexpr size_t MIN_REQUIRED_LOCAL_VIEWS = 8;
constexpr size_t MAX_LOCAL_VIEWS = 8;

enum class ViewAssignmentMode : uint8_t {
    OnePerPlayer,
    TwoByOneGrid,
    TwoByTwoGrid,
    ThreeByTwoGrid,
    FourByTwoGrid,
};

enum class PlayerForm : uint8_t { Human, Wolf };
enum class TransformPhase : uint8_t { Stable, ToHuman, ToWolf };

enum class PlayerLifeState : uint8_t {
    Alive,
    Downed,
    Dead,
};

enum class FriendlyFireMode : uint8_t {
    Ignore,
    ContactNoDamage,
    Full,
};

struct PlayerFormState {
    PlayerForm current = PlayerForm::Human;
    PlayerForm desired = PlayerForm::Human;
    TransformPhase phase = TransformPhase::Stable;
    bool sensesActive = false;
    bool transformationLocked = false;
};

struct PlayerMidnaRuntime {
    bool riderVisible = true;
    bool fieldActive = false;
    std::array<fpc_ProcID, 10> fieldTargets{};
    uint8_t fieldTargetCount = 0;
};

struct HorseSlot {
    daHorse_c* actor = nullptr;
    fpc_ProcID actorId = fpcM_ERROR_PROCESS_ID_e;
    PlayerId owner = 0;
    bool unlocked = false;
    bool summoned = false;
    bool mounted = false;
    std::optional<cXyz> lastKnownPosition;
    std::optional<s16> lastKnownYaw;
    std::string lastKnownStage;
    int8_t lastKnownRoom = -1;
};

struct PlayerSlot {
    // Gate D: may be a lightweight proxy (fopAc_ac_c), not a full daAlink_c yet.
    fopAc_ac_c* actor = nullptr;
    PlayerId id = 0;
    std::optional<s32> device;  // SDL_JoystickID when available
    std::optional<ViewId> view;
    std::optional<uint8_t> legacyPadPort;
    bool joined = false;
    bool enabled = false;
    bool transitionAuthority = false;
};

struct CameraRoute {
    camera_class* process = nullptr;
    fpc_ProcID processId = fpcM_ERROR_PROCESS_ID_e;
    ViewId view = 0;
    std::vector<PlayerId> trackedPlayers;
    PlayerId inputOwner = 0;
    PlayerId attentionOwner = 0;

    // Sidecar play-info for view > 0 (never index original mCameraInfo[1]).
    s8 winId = 0;
    s8 player1Id = 0;
    s8 player2Id = static_cast<s8>(-1);
    u32 attentionStatus = 0;
    f32 zoomScale = 1.0f;
    f32 zoomForcus = 1.0f;
    char* paramFileName = nullptr;
    cXyz savedPos{};
    cXyz savedTarget{};
    f32 savedFovy = 45.0f;
    s16 savedBank = 0;
};

struct PlayerStatusSidecar {
    std::array<uint32_t, 4> statusWords{};
};

struct PlayerLoadout {
    u8 itemX = 0xFF;
    u8 itemY = 0xFF;
    u8 itemSelect = 0xFF;
    u8 sword = 0;
    u8 shield = 0;
    u8 armor = 0;
};

struct PlayerResources {
    s16 life = 12 * 4;  // quarter-hearts; vanilla start
    s16 maxLife = 12 * 4;
    u16 magic = 0;
    u16 maxMagic = 0;
    u16 arrows = 0;
    u16 maxArrows = 30;
    u8 pachinko = 0;
    std::array<u8, 3> bombCounts{};  // bags 0..2; bag types are global item slots
    u16 oil = 0;
    u16 maxOil = 0;
    s16 rupees = 0;
    s16 maxRupees = 300;
    // dItemNo_NONE_e (0xFF) = no bottle in slot; EMPTY_BOTTLE = unlocked empty
    std::array<u8, 4> bottleContents{0xFF, 0xFF, 0xFF, 0xFF};
    std::array<u8, 4> bottleQuantities{};  // bee child etc.
};

struct PlayerCombatState {
    fpc_ProcID lockOnTarget = fpcM_ERROR_PROCESS_ID_e;
    u16 invulnerabilityFrames = 0;
    bool finisherClaimed = false;
    u16 cutType = 0;  // sidecar for proxy / non-daAlink bodies
};

struct PlayerRuntime {
    PlayerLoadout loadout;
    PlayerResources resources;
    PlayerCombatState combat;
    PlayerStatusSidecar status;
    PlayerLifeState lifeState = PlayerLifeState::Alive;
};

struct ContextFrame {
    PlayerId player = 0;
    ViewId view = 0;
    fopAc_ac_c* enemyTarget = nullptr;
};

struct Runtime {
    bool enabled = false;
    std::array<PlayerSlot, MAX_LOCAL_PLAYERS> players;
    std::array<PlayerRuntime, MAX_LOCAL_PLAYERS> playerRuntime;
    std::array<PlayerFormState, MAX_LOCAL_PLAYERS> forms;
    std::array<PlayerMidnaRuntime, MAX_LOCAL_PLAYERS> midna;
    std::array<HorseSlot, MAX_LOCAL_PLAYERS> horses;
    std::array<CameraRoute, MAX_LOCAL_VIEWS> cameras;
    // Window sidecars are owned by render/camera modules; stored as opaque indices here.
    PlayerId activePlayer = 0;
    ViewId activeView = 0;
    fopAc_ac_c* activeEnemyTarget = nullptr;
    uint8_t joinedPlayerCount = 1;
    uint8_t activeViewCount = 1;
    ViewAssignmentMode viewMode = ViewAssignmentMode::OnePerPlayer;
    FriendlyFireMode friendlyFire = FriendlyFireMode::Ignore;
};

}  // namespace dusk::coop
