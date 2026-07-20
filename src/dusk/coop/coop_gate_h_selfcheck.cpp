/**
 * Gate H — Q16.16 / snapshot self-check helpers.
 *
 * Expectations align with impl/common/example_encounter.md (2p Veteran, 4 enemies).
 * Called from difficulty::init / drops::init (not static constructors).
 */

#include "dusk/coop/coop_difficulty.h"
#include "dusk/coop/coop_drops.h"
#include "dusk/coop/coop_debug.h"
#include "dusk/coop/coop_gate_h_selfcheck.h"

namespace dusk::coop::gate_h {

bool runSelfChecks() {
    char err[128];
    if (!difficulty::selfCheck(err, sizeof(err))) {
        debug::logError("gate_H Q16 selfCheck: %s", err);
        return false;
    }

    const difficulty::Profile saved = difficulty::currentProfile();
    difficulty::setProfile(difficulty::Profile::Veteran);
    const auto& ps = difficulty::scalars();
    const difficulty::Q16_16 count = difficulty::compoundEnemyCount(2, ps);
    const u32 desired = difficulty::desiredEnemyCount(4, count);
    if (desired != 8) {
        debug::logError("gate_H: desiredEnemyCount 2p Veteran 4→%u (want 8)", desired);
        difficulty::setProfile(saved);
        return false;
    }

    const drops::EncounterSnapshot a =
        drops::makeSnapshot(2, difficulty::Profile::Veteran, 4, 0xC00Full);
    const drops::EncounterSnapshot b =
        drops::makeSnapshot(2, difficulty::Profile::Veteran, 4, 0xC00Full);
    if (a.countMul != b.countMul || a.budget.healing != b.budget.healing ||
        a.budget.ammo != b.budget.ammo || a.budget.rupees != b.budget.rupees ||
        a.perEnemyHpMul != b.perEnemyHpMul) {
        debug::logError("gate_H: snapshot not reproducible");
        difficulty::setProfile(saved);
        return false;
    }

    // Hearts: 4 × 1.25 × 0.40 = 2.00
    if (a.budget.healing != 2) {
        debug::logWarn("gate_H: heart budget=%u (example expects 2)", a.budget.healing);
    }

    // Stagger: 1.25 × 1.30 = 1.625
    const difficulty::Q16_16 stag = difficulty::compoundStagger(2, ps);
    if (stag < difficulty::fromFloat(1.624f) || stag > difficulty::fromFloat(1.626f)) {
        debug::logError("gate_H: stagger compound off");
        difficulty::setProfile(saved);
        return false;
    }

    difficulty::setProfile(saved);
    debug::logInfo("gate_H: Q16 + snapshot self-checks passed");
    return true;
}

}  // namespace dusk::coop::gate_h
