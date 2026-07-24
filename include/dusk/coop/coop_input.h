#pragma once

#include "dusk/coop/coop_types.h"

namespace dusk::coop::input {

struct StickState {
    f32 x = 0.0f;
    f32 y = 0.0f;
};

struct PlayerInputSnapshot {
    PlayerId player = 0;
    bool connected = false;
    bool reserved = false;  // disconnect keeps slot
    StickState leftStick;
    StickState rightStick;
    f32 leftTrigger = 0.0f;
    f32 rightTrigger = 0.0f;
    u16 buttonsHeld = 0;
    u16 buttonsPressed = 0;
    u16 buttonsReleased = 0;
    std::optional<s32> deviceId;
    std::optional<uint8_t> legacyPadPort;
};

void init();
void reset();
void tick();

const PlayerInputSnapshot& snapshot(PlayerId id);
bool tryJoinFromStartPress();
bool assignDevice(PlayerId id, s32 deviceId);
bool clearDevice(PlayerId id);
bool rumble(PlayerId id, f32 low, f32 high, u32 durationMs);

bool actionBindingValid(PlayerId id);
void onDeviceDisconnect(s32 deviceId);
void onDeviceReconnect(s32 deviceId);

bool isDeviceAssigned(s32 deviceId);
std::optional<PlayerId> playerForDevice(s32 deviceId);

}  // namespace dusk::coop::input
