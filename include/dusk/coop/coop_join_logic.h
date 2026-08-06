#pragma once

/**
 * \file coop_join_logic.h
 * M4 (D6) — join-warp policy, game-free core shared with the selftest.
 *
 * The mid-game join warp is UNLOCK-GATED (implementation-plan Rev 3 D6): a
 * client only warps to the host's stage when its own save has provably been
 * there; otherwise it REFUSES and pins a safe anchor (stays on its current
 * stage at its current position — the session continues, but the players do
 * not see each other until they are in a shared stage, which the stage-scoped
 * sender gate already handles).
 *
 * "Provably been there" = the SafeStageSet: stages the local real Link has
 * entered during this play session, plus the stage the save loaded from. TP's
 * save has no per-stage 'reached' bit (dSv_save_c memory entries are
 * initialized empty for all 32 stages), so this session-learned set is the
 * honest signal the engine exposes. The engine itself never crashes entering
 * a first-visited stage (normal progression does it); the refuse path exists
 * for the PLAYER EXPERIENCE — a fresh Ordon client teleported into a
 * mid-dungeon host would be lost, not broken.
 */

#include "dolphin/types.h"

#include <cstring>

namespace dusk::coop {

constexpr u8 kSafeStageMax = 24;
constexpr u8 kSafeStageNameLength = 16;

/// Stages this save has provably reached. Small fixed set; entries are
/// NUL-terminated stage names ("F_SP102").
class SafeStageSet {
public:
    bool Contains(const char* stage) const {
        if (stage == nullptr || stage[0] == '\0') {
            return false;
        }
        for (u8 i = 0; i < count_; ++i) {
            if (std::strcmp(entries_[i], stage) == 0) {
                return true;
            }
        }
        return false;
    }

    void Add(const char* stage) {
        if (stage == nullptr || stage[0] == '\0' || count_ >= kSafeStageMax) {
            return;
        }
        if (Contains(stage)) {
            return;
        }
        std::strncpy(entries_[count_], stage, kSafeStageNameLength - 1);
        entries_[count_][kSafeStageNameLength - 1] = '\0';
        ++count_;
    }

    [[nodiscard]] u8 Count() const { return count_; }

private:
    char entries_[kSafeStageMax][kSafeStageNameLength] = {};
    u8 count_ = 0;
};

enum class JoinWarpDecision : u8 {
    NoWarp,       // no target stage, or we are already in it: stay put
    Warp,         // target reached by this save: warp to the host's stage
    RefuseAnchor, // target NOT reached: refuse the warp, stay at our anchor
};

/// Pure decision (D6): the client warps to the host's stage only when its
/// save has reached it; a different-stage host in an un-reached stage is
/// refused and the client pins its current position.
inline JoinWarpDecision DecideJoinWarp(const char* targetStage, const char* myStage,
                                       const SafeStageSet& reached) {
    if (targetStage == nullptr || targetStage[0] == '\0') {
        return JoinWarpDecision::NoWarp;
    }
    if (myStage != nullptr && std::strcmp(targetStage, myStage) == 0) {
        return JoinWarpDecision::NoWarp;
    }
    return reached.Contains(targetStage) ? JoinWarpDecision::Warp
                                         : JoinWarpDecision::RefuseAnchor;
}

}  // namespace dusk::coop
