#include "dusk/coop/coop_debug.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_forms.h"
#include "dusk/coop/coop_input.h"

#include "d/d_com_inf_game.h"

#include <cstdarg>
#include <cstdio>

namespace dusk::coop::debug {

void init() {}
void reset() {}
void drawOverlay() {
    if (!isCompiledIn()) {
        return;
    }
    // Compact stdout telemetry for Gate C/I bring-up (ImGui overlay later).
    static int s_frameCounter = 0;
    if ((++s_frameCounter % 60) != 0) {
        return;
    }
    if (!isEnabled()) {
        return;
    }
    const u8 globalTf = dComIfGs_getTransformStatus();
    logInfo("forms global_save_tf=%u stage_rule=%u", globalTf,
            static_cast<unsigned>(forms::stageRule()));
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        if (!isJoined(id) && id != 0) {
            continue;
        }
        const auto& snap = input::snapshot(id);
        const auto& fs = forms::state(id);
        logInfo(
            "P%u connected=%d form=%s senses=%d stick=(%.2f,%.2f) buttons=0x%04x", id,
            snap.connected ? 1 : 0, fs.current == PlayerForm::Wolf ? "wolf" : "human",
            fs.sensesActive ? 1 : 0, snap.leftStick.x, snap.leftStick.y, snap.buttonsHeld);
    }
}

void logInfo(const char* fmt, ...) {
    std::fputs("[coop] ", stdout);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stdout, fmt, args);
    va_end(args);
    std::fputc('\n', stdout);
}

void logWarn(const char* fmt, ...) {
    std::fputs("[coop:warn] ", stderr);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
}

void logError(const char* fmt, ...) {
    std::fputs("[coop:error] ", stderr);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
}

}  // namespace dusk::coop::debug
