#include "dusk/coop/coop_dialogue_bridge.h"

#if TARGET_PC

#include "m_Do/m_Do_controller_pad.h"
#include "dusk/coop/coop.h"
#include "dusk/coop/coop_render.h"

extern "C" {

u8 dusk_coop_resolve_dialogue_pad(void) {
    if (dusk::coop::render::isConversationPresentationActive()) {
        const dusk::coop::PlayerId owner =
            dusk::coop::render::getConversationPresentationOwner();
        if (owner < dusk::coop::MAX_LOCAL_PLAYERS &&
            dusk::coop::isJoined(owner)) {
            dusk::coop::PlayerSlot* slot = dusk::coop::playerSlot(owner);
            if (slot != nullptr && slot->legacyPadPort.has_value()) {
                return *slot->legacyPadPort;
            }
        }
    }
    return PAD_1;  // fallback = port 0 (vanilla P1)
}

}  // extern "C"

#endif  // TARGET_PC