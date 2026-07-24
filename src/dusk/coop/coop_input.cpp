#include "dusk/coop/coop_input.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_accessors.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_player.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <unordered_map>

#if TARGET_PC
#include "aurora/lib/input.hpp"
#include "dolphin/pad.h"
#include "m_Do/m_Do_controller_pad.h"
#endif

namespace dusk::coop::input {
namespace {

std::array<PlayerInputSnapshot, MAX_LOCAL_PLAYERS> g_snapshots{};

#if TARGET_PC

struct DeviceIdentity {
    std::string guid;
    std::string serial;
};

std::array<DeviceIdentity, MAX_LOCAL_PLAYERS> g_identities{};
std::unordered_map<s32, bool> g_prevUnboundStart{};

constexpr u16 kClassicPadMask = static_cast<u16>(PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT | PAD_BUTTON_DOWN |
                                                   PAD_BUTTON_UP | PAD_TRIGGER_Z | PAD_TRIGGER_R | PAD_TRIGGER_L |
                                                   PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y |
                                                   PAD_BUTTON_START);

bool identitiesMatch(const DeviceIdentity& saved, const DeviceIdentity& current) {
    if (saved.guid.empty() || current.guid.empty()) {
        return false;
    }
    if (saved.guid == current.guid) {
        return saved.serial.empty() || current.serial.empty() || saved.serial == current.serial;
    }
    return !saved.serial.empty() && saved.serial == current.serial;
}

DeviceIdentity identityForDevice(s32 deviceId) {
    return DeviceIdentity{
        .guid = aurora::input::controller_guid(deviceId),
        .serial = aurora::input::controller_serial(deviceId),
    };
}

void applyEdgeButtons(PlayerInputSnapshot& snap, u16 held) {
    const u16 prev = snap.buttonsHeld;
    snap.buttonsHeld = held;
    snap.buttonsPressed = static_cast<u16>(held & ~prev);
    snap.buttonsReleased = static_cast<u16>(prev & ~held);
}

void clearAnalog(PlayerInputSnapshot& snap) {
    snap.leftStick = {};
    snap.rightStick = {};
    snap.leftTrigger = 0.0f;
    snap.rightTrigger = 0.0f;
    applyEdgeButtons(snap, 0);
}

bool controllerReportsJoinButton(aurora::input::GameController* ctrl, bool* outBackOnly) {
    if (outBackOnly != nullptr) {
        *outBackOnly = false;
    }
    if (ctrl == nullptr || ctrl->m_controller == nullptr) {
        return false;
    }

    const bool startHeld = SDL_GetGamepadButton(ctrl->m_controller, SDL_GAMEPAD_BUTTON_START);
    const bool backHeld = SDL_GetGamepadButton(ctrl->m_controller, SDL_GAMEPAD_BUTTON_BACK);

    bool mappedStart = false;
    for (const auto& mapping : ctrl->m_buttonMapping) {
        if (mapping.padButton != PAD_BUTTON_START) {
            continue;
        }
        if (SDL_GetGamepadButton(ctrl->m_controller,
                                 static_cast<SDL_GamepadButton>(mapping.nativeButton))) {
            mappedStart = true;
            break;
        }
    }

    const bool joinHeld = startHeld || mappedStart || backHeld;
    if (outBackOnly != nullptr) {
        *outBackOnly = joinHeld && !startHeld && !mappedStart && backHeld;
    }
    return joinHeld;
}

bool deviceIsJoinCandidate(s32 deviceId) {
    const auto owner = playerForDevice(deviceId);
    if (!owner.has_value()) {
        return true;
    }
    return !isJoined(*owner);
}

// Keep join-candidate pads out of every legacy PAD channel (especially port 0).
// Otherwise their Start feeds mDoCPd and opens the in-game start menu.
void unbindUnassignedControllers() {
    for (const SDL_JoystickID instance : aurora::input::controller_instances()) {
        const s32 id = static_cast<s32>(instance);
        if (!deviceIsJoinCandidate(id)) {
            continue;
        }
        const Sint32 port = aurora::input::player_index(static_cast<Uint32>(instance));
        if (port < 0) {
            continue;
        }
        if (port < static_cast<Sint32>(PAD_CHANMAX) &&
            aurora::input::get_instance_for_player(static_cast<uint32_t>(port)) == instance) {
            PADClearPort(static_cast<u32>(port));
        } else {
            aurora::input::set_player_index(static_cast<Uint32>(instance), -1);
        }
    }
}

void scrubJoinStartFromLegacyPads() {
    for (u32 port = 0; port < PAD_CHANMAX; ++port) {
        auto& info = mDoCPd_c::getCpadInfo(port);
        info.mButtonFlags &= ~PAD_BUTTON_START;
        info.mPressedButtonFlags &= ~PAD_BUTTON_START;
    }
}

bool unboundPadHoldingJoinButton() {
    for (const SDL_JoystickID instance : aurora::input::controller_instances()) {
        if (!deviceIsJoinCandidate(static_cast<s32>(instance))) {
            continue;
        }
        if (controllerReportsJoinButton(aurora::input::get_controller(instance), nullptr)) {
            return true;
        }
    }
    return false;
}

void reconcilePrimaryDevice() {
    auto* slot = playerSlot(0);
    if (slot == nullptr) {
        return;
    }

    unbindUnassignedControllers();

    if (slot->device.has_value()) {
        return;
    }

    // P0 gets the controller at PAD port 0 — the conventional Player 1 slot.
    // Query through Aurora rather than raw SDL to keep all input routed via Aurora.
    const Sint32 jsId = aurora::input::get_instance_for_player(0);
    if (jsId >= 0) {
        const s32 deviceId = static_cast<s32>(jsId);
        if (!isDeviceAssigned(deviceId) && assignDevice(0, deviceId)) {
            debug::logInfo("P0 claimed gamepad at PAD port 0 (device %d)", deviceId);
        }
    }
}

void sampleFromLegacyPad(PlayerId id) {
    auto& snap = g_snapshots[id];
    if (!snap.legacyPadPort.has_value()) {
        return;
    }
    const u32 port = *snap.legacyPadPort;
    if (port >= PAD_CHANMAX) {
        return;
    }

    const auto& pad = mDoCPd_c::getCpadInfo(port);
    const bool connected = mDoCPd_c::isConnect(port);
    snap.connected = connected && (snap.reserved || isJoined(id) || id == 0);

    if (!snap.connected) {
        clearAnalog(snap);
        return;
    }

    snap.leftStick.x = pad.mMainStickPosX;
    snap.leftStick.y = pad.mMainStickPosY;
    snap.rightStick.x = pad.mCStickPosX;
    snap.rightStick.y = pad.mCStickPosY;
    snap.leftTrigger = pad.mTriggerLeft;
    snap.rightTrigger = pad.mTriggerRight;
    applyEdgeButtons(snap, static_cast<u16>(pad.mButtonFlags & kClassicPadMask));

    // Only mirror formally assigned devices. Never auto-write slot->device here —
    // that made Press-Start treat unbound pads as already owned and ignore join.
    auto* slot = playerSlot(id);
    if (slot && slot->device.has_value()) {
        snap.deviceId = *slot->device;
        g_identities[id] = identityForDevice(*slot->device);
    } else {
        snap.deviceId.reset();
    }
}

void samplePlayer(PlayerId id) {
    auto& snap = g_snapshots[id];
    snap.player = id;

    if (!snap.connected && !snap.reserved && id != 0 && !isJoined(id)) {
        clearAnalog(snap);
        return;
    }

    sampleFromLegacyPad(id);
}

PlayerId findFreeJoinSlot() {
    // Undo ghost joins: room unload used to mark P1–P7 joined while co-op was still off
    // (getPlayerActor fell back to Link 0). Those slots have no pad and no actor.
    for (PlayerId id = 1; id < MAX_LOCAL_PLAYERS; ++id) {
        auto* slot = playerSlot(id);
        if (slot == nullptr || !slot->joined) {
            continue;
        }
        if (slot->device.has_value() || getPlayerActor(id) != nullptr) {
            continue;
        }
        if (g_snapshots[id].reserved && g_snapshots[id].deviceId.has_value()) {
            continue;  // real disconnect reservation
        }
        slot->joined = false;
        slot->enabled = false;
        slot->view.reset();
        g_snapshots[id].reserved = false;
        g_snapshots[id].connected = false;
        g_snapshots[id].deviceId.reset();
        debug::logInfo("Cleared ghost co-op slot P%u (joined with no device/actor)", id);
    }

    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        if (isJoined(id) || g_snapshots[id].reserved) {
            continue;
        }
        return id;
    }
    return MAX_LOCAL_PLAYERS;
}

bool claimLegacyPort(PlayerId id, s32 deviceId) {
    const u32 count = PADCount();
    for (u32 index = 0; index < count; ++index) {
        SDL_Gamepad* pad = PADGetSDLGamepadForIndex(index);
        if (pad == nullptr) {
            continue;
        }
        const SDL_JoystickID joystickId = SDL_GetJoystickID(SDL_GetGamepadJoystick(pad));
        if (static_cast<s32>(joystickId) == deviceId) {
            PADSetPortForIndex(index, id);
            return true;
        }
    }

    // Fallback: set SDL player index directly without persisting if index lookup failed.
    aurora::input::set_player_index(static_cast<Uint32>(deviceId), static_cast<Sint32>(id));
    return true;
}

void detectDisconnectsAndReconnects() {
    // Disconnect: assigned device vanished from Aurora registry.
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        if (!g_snapshots[id].deviceId.has_value()) {
            continue;
        }
        const s32 deviceId = *g_snapshots[id].deviceId;
        if (!aurora::input::controller_connected(deviceId)) {
            onDeviceDisconnect(deviceId);
        }
    }

    // Reconnect: new/returning instance matches a reserved identity.
    for (const SDL_JoystickID instance : aurora::input::controller_instances()) {
        if (isDeviceAssigned(instance)) {
            continue;
        }
        onDeviceReconnect(instance);
    }
}

#endif  // TARGET_PC

}  // namespace

void init() {
    g_snapshots = {};
#if TARGET_PC
    g_identities = {};
    g_prevUnboundStart.clear();
#endif
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        g_snapshots[i].player = i;
        g_snapshots[i].legacyPadPort = static_cast<uint8_t>(i);
        if (auto* slot = playerSlot(i)) {
            slot->legacyPadPort = static_cast<uint8_t>(i);
        }
    }
    g_snapshots[0].connected = true;
    g_snapshots[0].reserved = true;
#if TARGET_PC
    if (auto* slot = playerSlot(0)) {
        slot->joined = true;
        slot->enabled = true;
        g_snapshots[0].connected = true;
        g_snapshots[0].reserved = true;
    }
    {
        const auto pads = aurora::input::controller_instances();
        debug::logInfo("co-op input ready: %zu gamepad(s) visible", pads.size());
    }
#endif
}

void reset() { init(); }

void tick() {
#if !TARGET_PC
    return;
#else
    reconcilePrimaryDevice();
    detectDisconnectsAndReconnects();

    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        samplePlayer(id);
    }

    tryJoinFromStartPress();

    // Leave join-candidate pads Aurora-unowned so the *next* PADRead cannot map their
    // Start onto legacy port 0 (pause menu) before this tick runs.
    unbindUnassignedControllers();
#endif
}

const PlayerInputSnapshot& snapshot(PlayerId id) {
    static PlayerInputSnapshot invalid{};
    if (!isValidPlayer(id)) {
        return invalid;
    }
    return g_snapshots[id];
}

bool tryJoinFromStartPress() {
#if !TARGET_PC
    return false;
#else
    auto tryJoinDevice = [&](s32 instance, const char* source) -> bool {
        if (instance < 0) {
            return false;
        }

        if (const auto owner = playerForDevice(instance); owner.has_value()) {
            if (*owner == 0) {
                static s32 s_lastWarnedDevice = -1;
                if (s_lastWarnedDevice != instance) {
                    debug::logWarn(
                        "Start on device %d ignored — that pad is Player 0. Connect a second "
                        "gamepad and press Start to join",
                        instance);
                    s_lastWarnedDevice = instance;
                }
                return false;
            } else if (isJoined(*owner)) {
                return false;  // Already owned by another joined player.
            } else {
                // Ghost ownership (device recorded without join) — free it for claim.
                clearDevice(*owner);
            }
        }

        const PlayerId slot = findFreeJoinSlot();
        if (slot >= MAX_LOCAL_PLAYERS) {
            debug::logWarn("Start on device %d (%s): no free co-op slots", instance, source);
            return false;
        }

        if (!assignDevice(slot, instance)) {
            debug::logWarn("Start on device %d (%s): assignDevice(P%u) failed", instance, source,
                           slot);
            return false;
        }

        if (slot != 0) {
            const bool ok = player::onPlayerJoined(slot);
            debug::logInfo("Player %u joined via Start (device %d, %s, spawn=%d)", slot, instance,
                           source, ok ? 1 : 0);
        } else {
            debug::logInfo("Player 0 claimed device %d via Start (%s)", instance, source);
        }
        return true;
    };

    auto joinPressed = [](bool held, bool prev) -> bool { return held && !prev; };

    bool joinedAny = false;

    // Aurora-only join: poll SDL gamepads by instance. Do not use mDoCPd/getTrigStart —
    // unbound pads must never feed legacy PAD channels or Start opens the pause menu.
    for (const SDL_JoystickID instance : aurora::input::controller_instances()) {
        aurora::input::GameController* ctrl = aurora::input::get_controller(instance);
        if (ctrl == nullptr || ctrl->m_controller == nullptr) {
            g_prevUnboundStart.erase(instance);
            continue;
        }

        bool backOnly = false;
        const bool joinHeld = controllerReportsJoinButton(ctrl, &backOnly);
        const bool prevJoin = g_prevUnboundStart[instance];
        g_prevUnboundStart[instance] = joinHeld;
        if (!joinPressed(joinHeld, prevJoin)) {
            continue;
        }

        const char* source = backOnly ? "aurora-back" : "aurora-start";
        if (tryJoinDevice(static_cast<s32>(instance), source)) {
            joinedAny = true;
        }
    }

    // If an unbound pad is holding Start/Back (join intent), strip Start from legacy PAD
    // so this frame cannot open the in-game start menu.
    if (joinedAny || unboundPadHoldingJoinButton()) {
        scrubJoinStartFromLegacyPads();
    }

    for (auto it = g_prevUnboundStart.begin(); it != g_prevUnboundStart.end();) {
        if (!aurora::input::controller_connected(it->first)) {
            it = g_prevUnboundStart.erase(it);
        } else {
            ++it;
        }
    }

    return joinedAny;
#endif
}

bool assignDevice(PlayerId id, s32 deviceId) {
#if !TARGET_PC
    (void)id;
    (void)deviceId;
    return false;
#else
    auto* slot = playerSlot(id);
    if (!slot || !isValidPlayer(id)) {
        return false;
    }

    if (!aurora::input::controller_connected(deviceId)) {
        return false;
    }

    // One device → one player.
    if (const auto existing = playerForDevice(deviceId); existing.has_value() && *existing != id) {
        clearDevice(*existing);
    }

    if (!claimLegacyPort(id, deviceId)) {
        return false;
    }

    slot->device = deviceId;
    slot->id = id;
    slot->joined = true;
    slot->enabled = true;
    slot->legacyPadPort = static_cast<uint8_t>(id);
    g_snapshots[id].legacyPadPort = static_cast<uint8_t>(id);

    g_snapshots[id].deviceId = deviceId;
    g_snapshots[id].connected = true;
    g_snapshots[id].reserved = true;
    g_identities[id] = identityForDevice(deviceId);

    auto& rt = runtime();
    uint8_t joined = 0;
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (isJoined(i)) {
            ++joined;
        }
    }
    rt.joinedPlayerCount = joined;

    return true;
#endif
}

bool clearDevice(PlayerId id) {
    auto* slot = playerSlot(id);
    if (!slot || !isValidPlayer(id)) {
        return false;
    }
#if TARGET_PC
    if (g_snapshots[id].deviceId.has_value() && id < PAD_CHANMAX) {
        PADClearPort(id);
    }
    g_identities[id] = {};
#endif
    slot->device.reset();
    g_snapshots[id].deviceId.reset();
    g_snapshots[id].connected = false;
    // Keep reserved on disconnect — clearDevice is used for intentional unbind too.
    if (id == 0) {
        g_snapshots[id].reserved = true;
    } else {
        // Intentional unbind / ghost cleanup must free the slot for Press-Start join.
        g_snapshots[id].reserved = false;
    }
    return true;
}

bool rumble(PlayerId id, f32 low, f32 high, u32 durationMs) {
#if !TARGET_PC
    (void)id;
    (void)low;
    (void)high;
    (void)durationMs;
    return false;
#else
    if (!isJoined(id) || !g_snapshots[id].deviceId.has_value()) {
        return false;
    }
    const s32 instance = *g_snapshots[id].deviceId;
    if (!aurora::input::controller_has_rumble(static_cast<Uint32>(instance))) {
        return false;
    }
    const auto toU16 = [](f32 v) -> uint16_t {
        return static_cast<uint16_t>(std::clamp(v, 0.0f, 1.0f) * 65535.0f);
    };
    aurora::input::controller_rumble(static_cast<uint32_t>(instance), toU16(low), toU16(high),
                                     static_cast<uint16_t>(std::min<u32>(durationMs, 65535u)));
    return true;
#endif
}

bool actionBindingValid(PlayerId id) {
    return isValidPlayer(id) && isJoined(id);
}

void onDeviceDisconnect(s32 deviceId) {
#if !TARGET_PC
    (void)deviceId;
#else
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (g_snapshots[i].deviceId != deviceId) {
            continue;
        }
        g_snapshots[i].connected = false;
        g_snapshots[i].reserved = true;
        clearAnalog(g_snapshots[i]);
        // Keep deviceId + identity so reconnect can restore the same player.
        if (auto* slot = playerSlot(i)) {
            // Slot stays joined/reserved; device handle cleared until reconnect.
            slot->device.reset();
        }
        debug::logInfo("Player %u controller disconnected (device %d); slot reserved", i, deviceId);
    }
#endif
}

void onDeviceReconnect(s32 deviceId) {
#if !TARGET_PC
    (void)deviceId;
#else
    if (isDeviceAssigned(deviceId)) {
        return;
    }
    const DeviceIdentity current = identityForDevice(deviceId);

    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (!g_snapshots[i].reserved || g_snapshots[i].connected) {
            continue;
        }
        // Prefer exact prior deviceId, then GUID/serial identity.
        const bool idMatch = g_snapshots[i].deviceId.has_value() && *g_snapshots[i].deviceId == deviceId;
        const bool identityMatch = identitiesMatch(g_identities[i], current);
        if (!idMatch && !identityMatch) {
            continue;
        }
        if (assignDevice(i, deviceId)) {
            debug::logInfo("Player %u controller reconnected (device %d)", i, deviceId);
        }
        return;
    }
#endif
}

bool isDeviceAssigned(s32 deviceId) {
#if !TARGET_PC
    (void)deviceId;
    return false;
#else
    return playerForDevice(deviceId).has_value();
#endif
}

std::optional<PlayerId> playerForDevice(s32 deviceId) {
#if !TARGET_PC
    (void)deviceId;
    return std::nullopt;
#else
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (auto* slot = playerSlot(i);
            slot && slot->joined && slot->device.has_value() && *slot->device == deviceId) {
            return i;
        }
    }
    return std::nullopt;
#endif
}

}  // namespace dusk::coop::input
