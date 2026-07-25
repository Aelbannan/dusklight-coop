#include "dusk/coop/coop_save.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_bottles.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_forms.h"
#include "dusk/coop/coop_inventory.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace dusk::coop::save {
namespace {

#pragma pack(push, 1)
struct CompanionPlayerPayload {
    u8 playerId = 0;
    u8 joined = 0;
    u8 lifeState = 0;

    s16 life = 0;
    s16 maxLife = 0;
    u16 magic = 0;
    u16 maxMagic = 0;
    u16 arrows = 0;
    u16 maxArrows = 0;
    u8 pachinko = 0;
    u8 bombCounts[3]{};
    u16 oil = 0;
    u16 maxOil = 0;
    s16 rupees = 0;
    s16 maxRupees = 0;
    u8 bottleContents[4]{0xFF, 0xFF, 0xFF, 0xFF};
    u8 bottleQuantities[4]{};

    u8 itemX = 0xFF;
    u8 itemY = 0xFF;
    u8 itemSelect = 0xFF;
    u8 sword = 0;
    u8 shield = 0;
    u8 armor = 0;
    // Indexed form (0=human, 1=wolf).
    u8 form = 0;
};
#pragma pack(pop)

u32 crc32(const void* data, size_t len) {
    // Standard CRC-32 (IEEE), poly 0xEDB88320.
    u32 crc = 0xFFFFFFFFu;
    const auto* bytes = static_cast<const u8*>(data);
    for (size_t i = 0; i < len; ++i) {
        crc ^= bytes[i];
        for (int b = 0; b < 8; ++b) {
            const u32 mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

CompanionPlayerPayload packPlayer(PlayerId id) {
    CompanionPlayerPayload out{};
    out.playerId = id;
    out.joined = isJoined(id) ? 1 : 0;
    if (auto* rt = playerRuntime(id)) {
        out.lifeState = static_cast<u8>(rt->lifeState);
        const auto& res = rt->resources;
        out.life = res.life;
        out.maxLife = res.maxLife;
        out.magic = res.magic;
        out.maxMagic = res.maxMagic;
        out.arrows = res.arrows;
        out.maxArrows = res.maxArrows;
        out.pachinko = res.pachinko;
        for (int i = 0; i < 3; ++i) {
            out.bombCounts[i] = res.bombCounts[i];
        }
        out.oil = res.oil;
        out.maxOil = res.maxOil;
        out.rupees = res.rupees;
        out.maxRupees = res.maxRupees;
        for (int i = 0; i < 4; ++i) {
            out.bottleContents[i] = res.bottleContents[i];
            out.bottleQuantities[i] = res.bottleQuantities[i];
        }
        const auto& lo = rt->loadout;
        out.itemX = lo.itemX;
        out.itemY = lo.itemY;
        out.itemSelect = lo.itemSelect;
        out.sword = lo.sword;
        out.shield = lo.shield;
        out.armor = lo.armor;
    }
    out.form = static_cast<u8>(forms::state(id).current == PlayerForm::Wolf ? 1 : 0);
    return out;
}

void unpackPlayer(const CompanionPlayerPayload& in) {
    if (in.playerId >= MAX_LOCAL_PLAYERS) {
        return;
    }
    const PlayerId id = in.playerId;
    if (auto* slot = playerSlot(id)) {
        slot->joined = in.joined != 0;
        slot->enabled = in.joined != 0;
        slot->id = id;
    }
    if (auto* rt = playerRuntime(id)) {
        rt->lifeState = static_cast<PlayerLifeState>(in.lifeState);
        auto& res = rt->resources;
        res.life = in.life;
        res.maxLife = in.maxLife;
        res.magic = in.magic;
        res.maxMagic = in.maxMagic;
        res.arrows = in.arrows;
        res.maxArrows = in.maxArrows;
        res.pachinko = in.pachinko;
        for (int i = 0; i < 3; ++i) {
            res.bombCounts[i] = in.bombCounts[i];
        }
        res.oil = in.oil;
        res.maxOil = in.maxOil;
        res.rupees = in.rupees;
        res.maxRupees = in.maxRupees;
        for (int i = 0; i < 4; ++i) {
            res.bottleContents[i] = in.bottleContents[i];
            res.bottleQuantities[i] = in.bottleQuantities[i];
        }
        auto& lo = rt->loadout;
        lo.itemX = in.itemX;
        lo.itemY = in.itemY;
        lo.itemSelect = in.itemSelect;
        lo.sword = in.sword;
        lo.shield = in.shield;
        lo.armor = in.armor;
    }
    const PlayerForm form = (in.form != 0) ? PlayerForm::Wolf : PlayerForm::Human;
    auto& fs = forms::state(id);
    fs.current = form;
    fs.desired = form;
    fs.phase = TransformPhase::Stable;
}

bool readFully(FILE* f, void* dst, size_t n) {
    return std::fread(dst, 1, n, f) == n;
}

bool writeFully(FILE* f, const void* src, size_t n) {
    return std::fwrite(src, 1, n, f) == n;
}

PlayerId storyAuthority() {
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        if (auto* slot = playerSlot(id); slot != nullptr && slot->transitionAuthority) {
            return id;
        }
    }
    return 0;
}

}  // namespace

void init() {}
void reset() {}

std::string companionPathFor(const char* mainSavePath) {
    if (!mainSavePath || mainSavePath[0] == '\0') {
        return std::string();
    }
    return std::string(mainSavePath) + ".coop";
}

bool loadCompanion(const char* path) {
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        inventory::syncPlayerFromSave(id);
    }
    inventory::refreshGlobalItemsFromSave();
    bottles::syncUnlockedFromSave();

    if (path == nullptr) {
        recoverMissingCompanion();
        return false;
    }

    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        recoverMissingCompanion();
        return false;
    }

    CompanionHeader header{};
    if (!readFully(f, &header, sizeof(header))) {
        std::fclose(f);
        recoverCorruptCompanion("truncated header");
        return false;
    }

    if (header.magic != COMPANION_SAVE_MAGIC) {
        std::fclose(f);
        recoverCorruptCompanion("bad magic");
        return false;
    }
    if (header.version != COMPANION_SAVE_VERSION) {
        std::fclose(f);
        recoverCorruptCompanion("version mismatch");
        return false;
    }
    if (header.playerCount < 1 || header.playerCount > MAX_LOCAL_PLAYERS) {
        std::fclose(f);
        recoverCorruptCompanion("invalid playerCount");
        return false;
    }

    const u8 storedCount = header.playerCount;
    const u32 expectedPayload = static_cast<u32>(sizeof(CompanionPlayerPayload) * storedCount);
    if (header.payloadSize != expectedPayload) {
        std::fclose(f);
        recoverCorruptCompanion("payload size mismatch");
        return false;
    }

    std::vector<u8> payload(expectedPayload);
    if (expectedPayload > 0 && !readFully(f, payload.data(), expectedPayload)) {
        std::fclose(f);
        recoverCorruptCompanion("truncated payload");
        return false;
    }
    std::fclose(f);

    if (expectedPayload > 0) {
        const u32 crc = crc32(payload.data(), expectedPayload);
        if (crc != header.payloadCrc) {
            recoverCorruptCompanion("CRC mismatch");
            return false;
        }
    } else if (header.payloadCrc != 0) {
        recoverCorruptCompanion("CRC mismatch (empty payload)");
        return false;
    }

    for (u8 i = 0; i < storedCount; ++i) {
        CompanionPlayerPayload entry{};
        std::memcpy(&entry, payload.data() + i * sizeof(CompanionPlayerPayload),
                    sizeof(CompanionPlayerPayload));
        if (entry.playerId >= MAX_LOCAL_PLAYERS) {
            recoverCorruptCompanion("bad player id in payload");
            return false;
        }
        unpackPlayer(entry);
    }

    // Any joined player not present in the file is initialized from global progression.
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (!isJoined(i)) {
            continue;
        }
        bool found = false;
        for (u8 j = 0; j < storedCount; ++j) {
            CompanionPlayerPayload entry{};
            std::memcpy(&entry, payload.data() + j * sizeof(CompanionPlayerPayload),
                        sizeof(CompanionPlayerPayload));
            if (entry.playerId == i) {
                found = true;
                break;
            }
        }
        if (!found) {
            inventory::initPlayerFromProgression(i);
        }
    }

    inventory::syncPlayerToSave(storyAuthority());
    debug::logInfo("multiplayer save loaded (%s): players=%u", path, storedCount);
    return true;
}

bool saveCompanion(const char* path) {
    if (path == nullptr) {
        return false;
    }

    // Mirror the designated story authority into the vanilla save payload.
    inventory::syncPlayerToSave(storyAuthority());

    std::vector<CompanionPlayerPayload> players;
    players.reserve(MAX_LOCAL_PLAYERS);
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (!isJoined(i)) {
            continue;
        }
        players.push_back(packPlayer(i));
    }

    CompanionHeader header{};
    header.magic = COMPANION_SAVE_MAGIC;
    header.version = COMPANION_SAVE_VERSION;
    header.playerCount = static_cast<u8>(players.size());
    header.payloadSize = static_cast<u32>(sizeof(CompanionPlayerPayload) * players.size());
    header.payloadCrc =
        players.empty() ? 0 : crc32(players.data(), players.size() * sizeof(CompanionPlayerPayload));

    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        debug::logError("failed to open companion save for write: %s", path);
        return false;
    }

    bool ok = writeFully(f, &header, sizeof(header));
    if (ok && !players.empty()) {
        ok = writeFully(f, players.data(), players.size() * sizeof(CompanionPlayerPayload));
    }
    std::fclose(f);

    if (!ok) {
        debug::logError("failed writing companion save: %s", path);
        return false;
    }

    debug::logInfo("multiplayer save written (%s): players=%zu", path, players.size());
    return true;
}

void recoverMissingCompanion() {
    debug::logWarn(
        "multiplayer save missing; initializing joined players from global progression");
    inventory::refreshGlobalItemsFromSave();
    bottles::syncUnlockedFromSave();
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (!isJoined(i)) {
            continue;
        }
        inventory::initPlayerFromProgression(i);
    }
}

void recoverCorruptCompanion(const char* reason) {
    debug::logError("coop companion save corrupt (%s); reinitializing affected players",
                    reason ? reason : "unknown");
    recoverMissingCompanion();
}

}  // namespace dusk::coop::save
