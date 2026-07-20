#include "dusk/coop/coop_input.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_player.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <unordered_map>

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
#include "aurora/lib/input.hpp"
#include "dolphin/pad.h"
#include "m_Do/m_Do_controller_pad.h"
#endif

namespace dusk::coop::input {
namespace {

std::array<PlayerInputSnapshot, MAX_LOCAL_PLAYERS> g_snapshots{};

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC

struct DeviceIdentity {
    std::string guid;
    std::string serial;
};

std::array<DeviceIdentity, MAX_LOCAL_PLAYERS> g_identities{};
std::unordered_map<s32, u16> g_prevUnboundButtons{};
std::optional<PlayerId> g_keyboardPlayer{};

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

bool portHasKeyboard(u32 port) {
    u32 count = 0;
    return PADGetKeyButtonBindings(port, &count) != nullptr;
}

void enforceKeyboardPolicy() {
    std::optional<PlayerId> firstKeyboard;
    for (u32 port = 0; port < PAD_CHANMAX; ++port) {
        if (!portHasKeyboard(port)) {
            continue;
        }
        if (!firstKeyboard.has_value()) {
            firstKeyboard = static_cast<PlayerId>(port);
            continue;
        }
        // At most one keyboard/mouse player — clear extras.
        PADClearKeyBindings(port);
        PADSetKeyboardActive(port, FALSE);
        debug::logWarn("Keyboard/mouse already controls player %u; cleared port %u", *firstKeyboard, port);
    }
    g_keyboardPlayer = firstKeyboard;
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
    const bool connected = mDoCPd_c::isConnect(port) || portHasKeyboard(port);
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

    if (aurora::input::get_controller_for_player(port) != nullptr) {
        const Sint32 instance = aurora::input::get_instance_for_player(port);
        snap.deviceId = instance;
        if (auto* slot = playerSlot(id)) {
            slot->device = instance;
        }
        g_identities[id] = identityForDevice(instance);
    }
}

void sampleFromAuroraDevice(PlayerId id) {
    auto& snap = g_snapshots[id];
    if (!snap.deviceId.has_value()) {
        if (snap.reserved) {
            snap.connected = false;
            clearAnalog(snap);
        }
        return;
    }

    aurora::input::NormalizedControllerState state{};
    if (!aurora::input::read_controller(*snap.deviceId, &state) || !state.connected) {
        // Device missing — disconnect path owns reservation.
        snap.connected = false;
        clearAnalog(snap);
        return;
    }

    snap.connected = true;
    snap.leftStick.x = state.leftStickX;
    snap.leftStick.y = state.leftStickY;
    snap.rightStick.x = state.rightStickX;
    snap.rightStick.y = state.rightStickY;
    snap.leftTrigger = state.leftTrigger;
    snap.rightTrigger = state.rightTrigger;
    applyEdgeButtons(snap, static_cast<u16>(state.buttons & kClassicPadMask));
}

void samplePlayer(PlayerId id) {
    auto& snap = g_snapshots[id];
    snap.player = id;

    if (!snap.connected && !snap.reserved && id != 0 && !isJoined(id)) {
        clearAnalog(snap);
        return;
    }

    // Players 0-3 prefer the legacy PAD bridge (includes keyboard + remaps).
    if (snap.legacyPadPort.has_value()) {
        sampleFromLegacyPad(id);
        return;
    }

    // Players 4-7: snapshots only — no legacy PAD port.
    sampleFromAuroraDevice(id);
}

PlayerId findFreeJoinSlot() {
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        if (isJoined(id) || g_snapshots[id].reserved) {
            continue;
        }
        return id;
    }
    return MAX_LOCAL_PLAYERS;
}

bool claimLegacyPort(PlayerId id, s32 deviceId) {
    if (id >= PAD_CHANMAX) {
        // Keep Aurora player index unset so ports 0-3 stay undisturbed.
        aurora::input::set_player_index(static_cast<Uint32>(deviceId), -1);
        return true;
    }

    // Mirror Players 0-3 into legacy PAD ports 0-3.
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

#endif  // ENABLE_LOCAL_COOP && TARGET_PC

}  // namespace

void init() {
    g_snapshots = {};
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    g_identities = {};
    g_prevUnboundButtons.clear();
    g_keyboardPlayer.reset();
#endif
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        g_snapshots[i].player = i;
        if (i < 4) {
            g_snapshots[i].legacyPadPort = static_cast<uint8_t>(i);
            if (auto* slot = playerSlot(i)) {
                slot->legacyPadPort = static_cast<uint8_t>(i);
            }
        }
    }
    g_snapshots[0].connected = true;
    g_snapshots[0].reserved = true;
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    enforceKeyboardPolicy();
    if (aurora::input::get_controller_for_player(0) != nullptr) {
        assignDevice(0, aurora::input::get_instance_for_player(0));
    } else if (auto* slot = playerSlot(0)) {
        slot->joined = true;
        slot->enabled = true;
        g_snapshots[0].connected = true;
        g_snapshots[0].reserved = true;
    }
#endif
}

void reset() { init(); }

void tick() {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    return;
#else
    if (!isCompiledIn()) {
        return;
    }

    enforceKeyboardPolicy();
    detectDisconnectsAndReconnects();

    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        samplePlayer(id);
    }

    tryJoinFromStartPress();
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
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    return false;
#else
    if (!isCompiledIn()) {
        return false;
    }

    bool joinedAny = false;
    for (const SDL_JoystickID instance : aurora::input::controller_instances()) {
        if (isDeviceAssigned(instance)) {
            g_prevUnboundButtons.erase(instance);
            continue;
        }

        aurora::input::NormalizedControllerState state{};
        if (!aurora::input::read_controller(instance, &state)) {
            continue;
        }

        const u16 held = static_cast<u16>(state.buttons & kClassicPadMask);
        const u16 prev = g_prevUnboundButtons[instance];
        g_prevUnboundButtons[instance] = held;
        const u16 pressed = static_cast<u16>(held & ~prev);
        if ((pressed & PAD_BUTTON_START) == 0) {
            continue;
        }

        const PlayerId slot = findFreeJoinSlot();
        if (slot >= MAX_LOCAL_PLAYERS) {
            debug::logWarn("Start pressed on device %d but no free co-op slots", instance);
            continue;
        }

        if (!isEnabled()) {
            setEnabled(true);
        }

        if (!assignDevice(slot, instance)) {
            continue;
        }

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
        // Gate D: spawn proxy + camera follow for secondary joiners.
        if (slot != 0) {
            player::onPlayerJoined(slot);
        }
#endif

        debug::logInfo("Player %u joined via Start (device %d%s)", slot, instance,
                       slot >= PAD_CHANMAX ? ", snapshot-only" : "");
        joinedAny = true;
    }

    // Drop stale unbound tracking for removed devices.
    for (auto it = g_prevUnboundButtons.begin(); it != g_prevUnboundButtons.end();) {
        if (!aurora::input::controller_connected(it->first)) {
            it = g_prevUnboundButtons.erase(it);
        } else {
            ++it;
        }
    }

    return joinedAny;
#endif
}

bool assignDevice(PlayerId id, s32 deviceId) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)id;
    (void)deviceId;
    return false;
#else
    auto* slot = playerSlot(id);
    if (!slot || !isValidPlayer(id)) {
        return false;
    }

    const bool devicePresent = aurora::input::controller_connected(deviceId);
    if (!devicePresent) {
        // Allow player 0 without a gamepad (keyboard-only).
        if (!(id == 0 && portHasKeyboard(0))) {
            return false;
        }
        slot->device.reset();
        slot->id = id;
        slot->joined = true;
        slot->enabled = true;
        slot->legacyPadPort = 0;
        g_snapshots[id].deviceId.reset();
        g_snapshots[id].legacyPadPort = 0;
        g_snapshots[id].connected = true;
        g_snapshots[id].reserved = true;
        g_identities[id] = {};
        runtime().joinedPlayerCount = std::max(runtime().joinedPlayerCount, static_cast<uint8_t>(1));
        return true;
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
    if (id < PAD_CHANMAX) {
        slot->legacyPadPort = static_cast<uint8_t>(id);
        g_snapshots[id].legacyPadPort = static_cast<uint8_t>(id);
    } else {
        slot->legacyPadPort.reset();
        g_snapshots[id].legacyPadPort.reset();
    }

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
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
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
    }
    return true;
}

bool rumble(PlayerId id, f32 low, f32 high, u32 durationMs) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
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
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
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
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
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
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)deviceId;
    return false;
#else
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (g_snapshots[i].connected && g_snapshots[i].deviceId == deviceId) {
            return true;
        }
    }
    return false;
#endif
}

std::optional<PlayerId> playerForDevice(s32 deviceId) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)deviceId;
    return std::nullopt;
#else
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (g_snapshots[i].deviceId == deviceId) {
            return i;
        }
    }
    return std::nullopt;
#endif
}

std::optional<PlayerId> keyboardPlayer() {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    return std::nullopt;
#else
    return g_keyboardPlayer;
#endif
}

}  // namespace dusk::coop::input
