# Controller Input — Design

## Preserve Aurora ownership

Aurora already tracks connected controllers dynamically by SDL identity. Dusklight must not open duplicate SDL gamepad handles.

Add/upstream a narrow Aurora API:

```cpp
namespace aurora::input {
    std::vector<SDL_JoystickID> controller_instances();
    bool read_controller(SDL_JoystickID instance, NormalizedControllerState* out);
    bool controller_rumble(SDL_JoystickID instance, uint16_t low, uint16_t high, uint32_t durationMs);
}
```

## Player input snapshot

```cpp
struct PlayerInputSnapshot {
    uint32_t held = 0, pressed = 0, released = 0;
    float moveX = 0.0f, moveY = 0.0f;
    float lookX = 0.0f, lookY = 0.0f;
    float leftTrigger = 0.0f, rightTrigger = 0.0f;
    bool connected = false;
};
```

All co-op-aware Link and camera code consumes this snapshot, including Players 0-3. Legacy bridge is for unmodified original systems only.

## Fixed four-port bridge

Keep `PAD_CHANMAX == 4`. Mirror Players 0-3 into ports 0-3. Players 4-7 have no legacy port.

## Action bindings

- Add player-ID range checks to every `getActionBind*` function
- Move runtime press state to `MAX_LOCAL_PLAYERS`
- Add variable-length binding configuration for secondary players
- Update settings UI and serialization
- Evaluate bindings from `PlayerInputSnapshot`, not by re-querying Aurora

## Keyboard policy

Default: one keyboard/mouse source may control one player. Other local players require gamepads. Split-keyboard profiles not supported initially.

## Join/reconnect

Press Start to join. Assignments stored in settings (not save):

```cpp
struct DevicePreference {
    PlayerId player;
    std::string guid;
    std::optional<std::string> serial;
};
```

On disconnect: keep slot reserved, neutralize input, pause if authority player, permit reconnection or reassignment.

## Rumble

Route through player-to-instance mapping. Never index `m_gamePad[pad]` with player IDs 4-7.
