# Core Co-op Runtime

Use a PC-only sidecar. Do not alter the memory layout of decompiled classes solely to fit co-op state.

## Runtime structure

```cpp
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
    daAlink_c* actor = nullptr;
    PlayerId id = 0;
    std::optional<SDL_JoystickID> device;
    std::optional<ViewId> view;
    std::optional<uint8_t> legacyPadPort;
    bool joined = false;
    bool enabled = false;
    bool transitionAuthority = false;
};

struct CameraRoute {
    camera_class* process = nullptr;
    ViewId view = 0;
    std::vector<PlayerId> trackedPlayers;
    PlayerId inputOwner = 0;
};

struct PlayerStatusSidecar {
    std::array<uint32_t, 4> statusWords{};
};

struct PlayerRuntime {
    PlayerLoadout loadout;
    PlayerResources resources;
    PlayerCombatState combat;
    PlayerStatusSidecar status;
    PlayerLifeState lifeState = PlayerLifeState::Alive;
};

struct Runtime {
    bool enabled = false;
    std::array<PlayerSlot, MAX_LOCAL_PLAYERS> players;
    std::array<PlayerRuntime, MAX_LOCAL_PLAYERS> playerRuntime;
    std::array<PlayerFormState, MAX_LOCAL_PLAYERS> forms;
    std::array<PlayerMidnaRuntime, MAX_LOCAL_PLAYERS> midna;
    std::array<HorseSlot, MAX_LOCAL_PLAYERS> horses;
    std::array<CameraRoute, MAX_LOCAL_VIEWS> cameras;
    std::array<dDlst_window_c, MAX_LOCAL_VIEWS> windows;
    PlayerId activePlayer = 0;
    ViewId activeView = 0;
    fopAc_ac_c* activeEnemyTarget = nullptr;
    uint8_t joinedPlayerCount = 1;
    uint8_t activeViewCount = 1;
    ViewAssignmentMode viewMode = ViewAssignmentMode::OnePerPlayer;
};

Runtime& runtime();

}
```

## Identifier rules

Do not conflate:

```text
Physical device ID
Player ID
Legacy PAD port
View ID
Camera process index
Actor process ID
```

All mappings are explicit.

## View policy

Every joined player receives an independent camera and viewport up to eight players.

Default layouts:

| Players | Grid | Unused |
|--------:|------|-------:|
| 1 | 1×1 | 0 |
| 2 | 2×1 horizontal | 0 |
| 3 | 2×2 | 1 |
| 4 | 2×2 | 0 |
| 5 | 3×2 | 1 |
| 6 | 3×2 | 0 |
| 7 | 4×2 | 1 |
| 8 | 4×2 | 0 |

For 16:9 output, a 4×2 grid gives each viewport 8:9 aspect. Each camera must use the actual viewport aspect and a co-op-specific framing policy.

## Context safety

Scoped contexts must be nestable and restored with RAII. In debug builds, store a context stack and assert empty at end of each tick and render pass.

```cpp
struct ContextFrame {
    PlayerId player;
    ViewId view;
    fopAc_ac_c* enemyTarget;
};
```

Do not use process-global mutable context from worker threads. Use thread-local context or explicit parameters.
