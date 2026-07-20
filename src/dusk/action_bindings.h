#pragma once

#include <unordered_map>

#include "dusk/config_var.hpp"

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
#include "dusk/coop/coop_types.h"
#endif

namespace dusk {

enum class ActionBinds {
    FIRST_PERSON_CAMERA,
    CALL_MIDNA,
    OPEN_MAP_SCREEN,
    TOGGLE_MINIMAP,
    OPEN_DUSKLIGHT_MENU,
    TURBO_SPEED_BUTTON,
    COUNT,
};

struct ActionBindData {
    std::array<config::ActionBindConfigVar, 4>* configVars{};
    std::string actionName{};
};

struct ActionBindPressData {
    bool pressedCurFrame{false};
    bool pressedPrevFrame{false};
};

using ActionBindsMap = std::unordered_map<ActionBinds, ActionBindData>;

ActionBindsMap& getActionBinds();

bool isActionBound(ActionBinds action, u32 port);

void updateActionBindings();

void setVirtualActionBind(ActionBinds action, u32 port, bool pressed, bool available = true);

void clearVirtualActionBind(ActionBinds action, u32 port);

void clearAllVirtualActionBinds();

bool getActionBindTrig(ActionBinds action, u32 port);

bool getActionBindHold(ActionBinds action, u32 port);

bool getActionBindHoldAnyPort(ActionBinds action);

int getActionBindButton(ActionBinds action, u32 port);

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
// Player-ID aware wrappers — reject invalid / unjoined players.
bool isActionBoundForPlayer(ActionBinds action, coop::PlayerId player);
bool getActionBindTrigForPlayer(ActionBinds action, coop::PlayerId player);
bool getActionBindHoldForPlayer(ActionBinds action, coop::PlayerId player);
int getActionBindButtonForPlayer(ActionBinds action, coop::PlayerId player);
#endif

}
