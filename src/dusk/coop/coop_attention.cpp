#include "dusk/coop/coop_attention.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_accessors.h"
#include "dusk/coop/coop_context.h"
#include "dusk/coop/coop_debug.h"

#include "d/d_attention.h"
#include "d/d_com_inf_game.h"

#include <array>

namespace dusk::coop::attention {
namespace {

// Every player (including P0) gets a dedicated attention instance.
// The original embedded mAttention in the play struct is unused in coop builds.
std::array<dAttention_c*, MAX_LOCAL_PLAYERS> g_attentions{};

dAttention_c* createInstance(PlayerId player) {
    fopAc_ac_c* actor = getPlayerActor(player);
    auto* attn = new dAttention_c(actor, static_cast<u32>(player));
    if (attn == nullptr) {
        debug::logError("attention: failed to allocate instance for P%u", player);
        return nullptr;
    }
    g_attentions[player] = attn;
    return attn;
}

}  // namespace

void init() {
    // Clean up any previous instances (P1+).
    for (PlayerId i = 1; i < MAX_LOCAL_PLAYERS; ++i) {
        if (g_attentions[i] != nullptr) {
            delete g_attentions[i];
            g_attentions[i] = nullptr;
        }
    }
    // P0 uses the original embedded instance that is created during normal
    // game startup (after the game heap and J3D resources are available).
    g_attentions[0] = g_dComIfG_gameInfo.play.getAttention();
}

void destroy() {
    for (PlayerId i = 1; i < MAX_LOCAL_PLAYERS; ++i) {
        if (g_attentions[i] != nullptr) {
            delete g_attentions[i];
            g_attentions[i] = nullptr;
        }
    }
    g_attentions[0] = nullptr;
}

void tick() {
    // Lazy-create instances for newly joined players.
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (isJoined(i) && g_attentions[i] == nullptr) {
            createInstance(i);
        }
        // Tear down instances for players who left.
        if (!isJoined(i) && g_attentions[i] != nullptr && i != 0) {
            delete g_attentions[i];
            g_attentions[i] = nullptr;
        }
    }

    // Run each joined player's attention system.
    for (PlayerId i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
        if (!isJoined(i)) {
            continue;
        }
        dAttention_c* attn = g_attentions[i];
        if (attn == nullptr) {
            continue;
        }
        // Keep the instance pointed at the correct player actor
        // (actor may be recreated after room transitions).
        fopAc_ac_c* actor = getPlayerActor(i);
        if (actor == nullptr) {
            continue;  // Link not spawned yet for this player
        }
        ScopedContext context({i, static_cast<ViewId>(i), nullptr});
        attn->Init(actor, static_cast<u32>(i));
        attn->Run();
    }
}

dAttention_c* forContext() {
    return forPlayer(currentPlayer());
}

dAttention_c* forPlayer(PlayerId player) {
    if (!isValidPlayer(player)) {
        return nullptr;
    }
    return g_attentions[player];
}

}  // namespace dusk::coop::attention
