#pragma once

#include <mods/api.h>

/*
 * The mod SDK imports this service automatically for mods built with FEATURES game; service-only
 * and asset-only mods do not require it.
 *
 * Major version is the game-code ABI epoch: it is bumped when game-visible struct or vtable layouts
 * change incompatibly (e.g. a TARGET_PC field added to an existing game struct). The loader's
 * ordinary version check then fails mods built against the old epoch with a clear message instead
 * of letting them corrupt memory.
 */
#define GAME_SERVICE_ID "dev.twilitrealm.dusklight.game"
/*
 * Dusklight fork hygiene: the co-op fork grew game-visible structs (per-slot
 * player state etc.) beyond upstream, so the game ABI epoch is bumped. Mods
 * built against the stock epoch fail the loader's version check with a clear
 * message instead of corrupting memory (see plan Rev 3 D11/D12: the SDK stays
 * available but unused by the co-op).
 */
#define GAME_SERVICE_MAJOR 2u
#define GAME_SERVICE_MINOR 0u

typedef struct GameService {
    ServiceHeader header;
} GameService;

#ifdef __cplusplus
#include <mods/service.hpp>

template <>
struct mods::ServiceTraits<GameService> {
    static constexpr const char* id = GAME_SERVICE_ID;
    static constexpr uint16_t major_version = GAME_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = GAME_SERVICE_MINOR;
};
#endif
