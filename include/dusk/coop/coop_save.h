#pragma once

#include "dusk/coop/coop_types.h"

#include <string>

namespace dusk::coop::save {

constexpr u32 COMPANION_SAVE_MAGIC = 0x434F4F50;  // 'COOP'
constexpr u32 COMPANION_SAVE_VERSION = 3;

struct CompanionHeader {
    u32 magic = COMPANION_SAVE_MAGIC;
    u32 version = COMPANION_SAVE_VERSION;
    u8 playerCount = 1;
    u8 reserved[3]{};
    u32 payloadSize = 0;
    u32 payloadCrc = 0;
};

void init();
void reset();

// Derive companion save path from the main save file path by appending ".coop".
// When the main save path is not available (e.g. PC card abstraction), callers
// may pass a path derived from dusk::ConfigPath instead.
std::string companionPathFor(const char* mainSavePath);

bool loadCompanion(const char* path);
bool saveCompanion(const char* path);

// Missing companion → initialize every joined player from global progression.
void recoverMissingCompanion();
// Corrupt companion → log + reinitialize affected players.
void recoverCorruptCompanion(const char* reason);

}  // namespace dusk::coop::save
