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
std::unordered_map<s32, bool> g_prevUnboundStart{};
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

    // Only track formally assigned devices. Auto-claiming the pad on player-index 0 onto
    // keyboard-driven P0 made Press-Start treat that pad as already owned and ignore join.
    auto* slot = playerSlot(id);
    if (slot && slot->device.has_value()) {
        snap.deviceId = *slot->device;
        g_identities[id] = identityForDevice(*slot->device);
    } else if (id == 0 && portHasKeyboard(0)) {
        snap.deviceId.reset();
    } else if (aurora::input::get_controller_for_player(port) != nullptr) {
        const Sint32 instance = aurora::input::get_instance_for_player(port);
        snap.deviceId = instance;
        if (slot != nullptr) {
            slot->device = instance;
        }
        g_identities[id] = identityForDevice(instance);
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

#endif  // ENABLE_LOCAL_COOP && TARGET_PC

}  // namespace

void init() {
    g_snapshots = {};
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    g_identities = {};
    g_prevUnboundStart.clear();
    g_keyboardPlayer.reset();
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
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    enforceKeyboardPolicy();
    // If keyboard already drives port 0, leave the first gamepad unbound so Press-Start
    // can claim it as Player 1. Otherwise bind the pad on player-index 0 to P0.
    if (portHasKeyboard(0)) {
        if (auto* slot = playerSlot(0)) {
            slot->joined = true;
            slot->enabled = true;
            g_snapshots[0].connected = true;
            g_snapshots[0].reserved = true;
        }
        debug::logInfo("P0 using keyboard; gamepads stay free for Press-Start join");
    } else if (aurora::input::get_controller_for_player(0) != nullptr) {
        assignDevice(0, aurora::input::get_instance_for_player(0));
        debug::logInfo("P0 bound to gamepad device %d; press Start on a second unbound pad to join",
                       aurora::input::get_instance_for_player(0));
    } else if (auto* slot = playerSlot(0)) {
        slot->joined = true;
        slot->enabled = true;
        g_snapshots[0].connected = true;
        g_snapshots[0].reserved = true;
        debug::logInfo("P0 joined with no gamepad yet");
    }
    {
        const auto pads = aurora::input::controller_instances();
        debug::logInfo("co-op input ready: %zu gamepad(s) visible", pads.size());
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

    auto tryJoinDevice = [&](s32 instance, const char* source) -> bool {
        if (instance < 0) {
            return false;
        }

        // Start on P0's pad: if keyboard can drive P0, hand the pad to a new join slot.
        if (const auto owner = playerForDevice(instance); owner.has_value()) {
            if (*owner == 0 && portHasKeyboard(0)) {
                clearDevice(0);
                debug::logInfo("Start on P0 pad: releasing gamepad for co-op join (P0 keeps keyboard)");
            } else if (*owner == 0) {
                static s32 s_lastWarnedDevice = -1;
                if (s_lastWarnedDevice != instance) {
                    debug::logWarn(
                        "Start on device %d ignored — that pad is Player 0. Connect a second "
                        "gamepad and press Start on it (or use keyboard for P0)",
                        instance);
                    s_lastWarnedDevice = instance;
                }
                return false;
            } else {
                return false;  // Already owned by another joined player.
            }
        }

        const PlayerId slot = findFreeJoinSlot();
        if (slot >= MAX_LOCAL_PLAYERS) {
            debug::logWarn("Start on device %d (%s): no free co-op slots", instance, source);
            return false;
        }

        if (!isEnabled()) {
            setEnabled(true);
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
    static std::unordered_map<s32, bool> s_prevBack;

    // Path A: all Aurora-owned gamepads (Start or Select/Back edge).
    for (const SDL_JoystickID instance : aurora::input::controller_instances()) {
        aurora::input::GameController* ctrl = aurora::input::get_controller(instance);
        if (ctrl == nullptr || ctrl->m_controller == nullptr) {
            g_prevUnboundStart.erase(instance);
            s_prevBack.erase(instance);
            continue;
        }

        const bool startHeld = SDL_GetGamepadButton(ctrl->m_controller, SDL_GAMEPAD_BUTTON_START);
        const bool prevStart = g_prevUnboundStart[instance];
        g_prevUnboundStart[instance] = startHeld;
        const bool startEdge = joinPressed(startHeld, prevStart);

        const bool backHeld = SDL_GetGamepadButton(ctrl->m_controller, SDL_GAMEPAD_BUTTON_BACK);
        const bool prevBack = s_prevBack[instance];
        s_prevBack[instance] = backHeld;
        const bool backEdge = joinPressed(backHeld, prevBack);

        if (!startEdge && !backEdge) {
            continue;
        }

        const char* source = backEdge && !startEdge ? "pad-back" : "pad-start";
        if (tryJoinDevice(static_cast<s32>(instance), source)) {
            joinedAny = true;
        }
    }

    // Path B: legacy PAD ports 1-3 (second pad often lands on player-index 1).
    for (u32 port = 1; port < PAD_CHANMAX; ++port) {
        if (!mDoCPd_c::isConnect(port)) {
            continue;
        }
        if (!mDoCPd_c::getTrigStart(port)) {
            continue;
        }
        if (aurora::input::get_controller_for_player(port) == nullptr) {
            continue;
        }
        const s32 instance = aurora::input::get_instance_for_player(port);
        if (tryJoinDevice(instance, "pad-port")) {
            joinedAny = true;
        }
    }

    // Drop stale unbound tracking for removed devices.
    for (auto it = g_prevUnboundStart.begin(); it != g_prevUnboundStart.end();) {
        if (!aurora::input::controller_connected(it->first)) {
            it = g_prevUnboundStart.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = s_prevBack.begin(); it != s_prevBack.end();) {
        if (!aurora::input::controller_connected(it->first)) {
            it = s_prevBack.erase(it);
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
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    if (g_snapshots[id].deviceId.has_value() && id < PAD_CHANMAX) {
        PADClearPort(id);
    }
    g_identities[id] = {};
#endif
    slot->device.reset();
    g_snapshots[id].deviceId.reset();
#if defined(ENABLE_LOCAL_COOP) && TARGET_PC
    // Keyboard-only P0 stays connected after releasing a gamepad for Press-Start join.
    g_snapshots[id].connected = (id == 0 && portHasKeyboard(0));
#else
    g_snapshots[id].connected = false;
#endif
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
    return playerForDevice(deviceId).has_value();
#endif
}

std::optional<PlayerId> playerForDevice(s32 deviceId) {
#if !(defined(ENABLE_LOCAL_COOP) && TARGET_PC)
    (void)deviceId;
    return std::nullopt;
#else
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (auto* slot = playerSlot(i); slot && slot->device.has_value() && *slot->device == deviceId) {
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
