#include "dusk/coop/coop_forms.h"

#include "dusk/coop/coop.h"
#include "dusk/coop/coop_accessors.h"
#include "dusk/coop/coop_context.h"
#include "dusk/coop/coop_debug.h"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_save.h"

#include <array>
#include <cstring>

namespace dusk::coop::forms {
namespace {

StageFormPolicy g_stage{};
std::array<PlayerForm, MAX_LOCAL_PLAYERS> g_formBackup{};
bool g_forcedDemoActive = false;

void syncFormFromLink(PlayerId id, daAlink_c* link) {
    if (!isValidPlayer(id) || link == nullptr) {
        return;
    }
    auto& s = state(id);
    const PlayerForm live = link->checkWolf() ? PlayerForm::Wolf : PlayerForm::Human;
    if (s.phase == TransformPhase::Stable) {
        s.current = live;
        s.desired = live;
    }
}

}  // namespace

void init() {
    g_stage = {};
    g_formBackup = {};
    g_forcedDemoActive = false;
}

void reset() {
    init();
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        runtime().forms[id] = {};
        runtime().midna[id] = {};
    }
}

void tick() {
    if (!isEnabled()) {
        return;
    }

    // Keep P0 sidecar aligned with the live Link actor flag.
    if (auto* link = daAlink_getAlinkActorClass()) {
        syncFormFromLink(0, link);
    }
    syncSensesFromAuthorityLink();

    // Apply stage Force* softly to sidecar desired form (visual transform still needs Link).
    if (g_stage.rule == FormRule::ForceWolf || g_stage.rule == FormRule::ForceHuman) {
        const PlayerForm forced =
            (g_stage.rule == FormRule::ForceWolf) ? PlayerForm::Wolf : PlayerForm::Human;
        if (g_stage.forbidAll) {
            for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
                if (isJoined(id)) {
                    setDesiredForm(id, forced);
                }
            }
        } else if (g_stage.eventParticipant >= 0 &&
                   isValidPlayer(static_cast<PlayerId>(g_stage.eventParticipant))) {
            setDesiredForm(static_cast<PlayerId>(g_stage.eventParticipant), forced);
        }
    }
}

PlayerFormState& state(PlayerId id) {
    COOP_ASSERT(isValidPlayer(id));
    return runtime().forms[id];
}

PlayerMidnaRuntime& midna(PlayerId id) {
    COOP_ASSERT(isValidPlayer(id));
    return runtime().midna[id];
}

bool isWolf(PlayerId id) {
    if (!isValidPlayer(id)) {
        return false;
    }
    if (id == 0) {
        if (auto* link = daAlink_getAlinkActorClass()) {
            return link->checkWolf() != 0;
        }
    }
    return state(id).current == PlayerForm::Wolf;
}

bool isCurrentContextPlayerWolf() {
    if (!isEnabled()) {
        return isStoryAuthorityWolf();
    }
    return isWolf(currentPlayer());
}

bool isStoryAuthorityWolf() {
    if (auto* link = daAlink_getAlinkActorClass()) {
        return link->checkWolf() != 0;
    }
    return dComIfGs_getTransformStatus() == TF_STATUS_WOLF;
}

bool isTransformUnlocked() {
    // Global progression unlock (event bit 0x0D04) — one source of truth.
    return dComIfGs_isEventBit(0x0D04) != 0;
}

bool setDesiredForm(PlayerId id, PlayerForm form) {
    if (!isValidPlayer(id)) {
        return false;
    }
    auto& s = state(id);
    if (s.transformationLocked) {
        return false;
    }
    if (!allowsForm(id, form)) {
        return false;
    }
    s.desired = form;
    return true;
}

bool beginTransform(PlayerId id, PlayerForm form) {
    if (!setDesiredForm(id, form)) {
        return false;
    }
    auto& s = state(id);
    s.phase = (form == PlayerForm::Wolf) ? TransformPhase::ToWolf : TransformPhase::ToHuman;
    writeTransformSaveIfAuthority(id, form);

    // Gate D proxy cannot run changeWolf — complete sidecar-only for secondaries so
    // concurrent form *state* is demonstrable; visual wolf body remains a blocker.
    if (id != 0) {
        onTransformComplete(id);
        debug::logInfo(
            "forms: P%u sidecar transform -> %s (proxy has no changeWolf; visual deferred)", id,
            form == PlayerForm::Wolf ? "wolf" : "human");
    }
    return true;
}

void onTransformComplete(PlayerId id) {
    if (!isValidPlayer(id)) {
        return;
    }
    auto& s = state(id);
    s.current = s.desired;
    s.phase = TransformPhase::Stable;
}

void writeTransformSaveIfAuthority(PlayerId id, PlayerForm form) {
    if (!isValidPlayer(id)) {
        return;
    }

    auto& s = state(id);
    s.desired = form;

    if (id != 0) {
        // Secondary players must not write dComIfGs_setTransformStatus.
        debug::logInfo("forms: P%u skip global transform save write (sidecar only)", id);
        return;
    }

    // Player 0: write the original global transform save field.
    // Call the underlying setter directly to avoid re-entering the co-op divert.
    g_dComIfG_gameInfo.info.getPlayer().getPlayerStatusA().setTransformStatus(
        form == PlayerForm::Wolf ? TF_STATUS_WOLF : TF_STATUS_HUMAN);
}

void notifyLinkFormChanged(daAlink_c* link, PlayerForm form) {
    const PlayerId id = playerIdForLink(link);
    writeTransformSaveIfAuthority(id, form);
    onTransformComplete(id);
    if (form == PlayerForm::Human) {
        setSenses(id, false);
    }
}

PlayerId playerIdForActor(const fopAc_ac_c* actor) {
    if (actor == nullptr) {
        return 0;
    }
    if (!isEnabled()) {
        return 0;
    }
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        if (isJoined(id) && getPlayerActor(id) == actor) {
            return id;
        }
    }
    // Unmapped Link-like actors default to story authority (P0).
    return 0;
}

PlayerId playerIdForLink(const daAlink_c* link) {
    return playerIdForActor(reinterpret_cast<const fopAc_ac_c*>(link));
}

bool sensesActiveForView(ViewId view) {
    if (!isValidView(view)) {
        return false;
    }
    auto* route = cameraRoute(view);
    if (!route) {
        return false;
    }
    return sensesActiveForPlayer(route->inputOwner);
}

bool sensesActiveForPlayer(PlayerId id) {
    if (!isValidPlayer(id) || !isJoined(id)) {
        return false;
    }
    return state(id).sensesActive && isWolf(id);
}

void setSenses(PlayerId id, bool active) {
    if (isValidPlayer(id)) {
        state(id).sensesActive = active;
    }
}

void syncSensesFromAuthorityLink() {
    auto* link = daAlink_getAlinkActorClass();
    if (link == nullptr) {
        return;
    }
    setSenses(0, link->checkWolfEyeUp() != 0 && link->checkWolf() != 0);
}

FormRule stageRule() { return g_stage.rule; }

const StageFormPolicy& stagePolicy() { return g_stage; }

void setStagePolicy(StageFormPolicy policy) { g_stage = policy; }

void clearStagePolicy() {
    g_stage = {};
    g_forcedDemoActive = false;
}

bool allowsForm(PlayerId id, PlayerForm form) {
    if (!isValidPlayer(id)) {
        return false;
    }
    switch (g_stage.rule) {
    case FormRule::Either:
        return true;
    case FormRule::NoTransformation:
        return state(id).current == form;
    case FormRule::ForceHuman:
        return form == PlayerForm::Human;
    case FormRule::ForceWolf: {
        if (g_stage.forbidAll) {
            return form == PlayerForm::Wolf;
        }
        if (g_stage.eventParticipant >= 0 &&
            static_cast<PlayerId>(g_stage.eventParticipant) == id) {
            return form == PlayerForm::Wolf;
        }
        // Non-participants keep Either semantics unless forbidAll.
        return true;
    }
    }
    return true;
}

void applyForcedFormDemo(PlayerId participant, PlayerForm form) {
    if (!isValidPlayer(participant)) {
        return;
    }
    if (!g_forcedDemoActive) {
        for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
            g_formBackup[id] = state(id).current;
        }
        g_forcedDemoActive = true;
    }
    StageFormPolicy policy{};
    policy.rule = (form == PlayerForm::Wolf) ? FormRule::ForceWolf : FormRule::ForceHuman;
    policy.eventParticipant = static_cast<int8_t>(participant);
    policy.forbidAll = false;
    setStagePolicy(policy);
    beginTransform(participant, form);
    debug::logInfo("forms: forced-form demo P%u -> %s", participant,
                   form == PlayerForm::Wolf ? "wolf" : "human");
}

void restoreFormsAfterForcedDemo() {
    if (!g_forcedDemoActive) {
        clearStagePolicy();
        return;
    }
    for (PlayerId id = 0; id < MAX_LOCAL_PLAYERS; ++id) {
        if (isJoined(id)) {
            beginTransform(id, g_formBackup[id]);
        }
    }
    g_forcedDemoActive = false;
    clearStagePolicy();
    debug::logInfo("forms: restored independent forms after forced demo");
}

}  // namespace dusk::coop::forms

#if defined(ENABLE_LOCAL_COOP) && TARGET_PC

extern "C" {

int dusk_coop_checkNowWolf(void) {
    if (!dusk::coop::isEnabled()) {
        daAlink_c* link = daAlink_getAlinkActorClass();
        return link != nullptr ? static_cast<int>(link->checkWolf()) : 0;
    }
    return dusk::coop::forms::isCurrentContextPlayerWolf() ? 1 : 0;
}

int dusk_coop_checkNowWolfAuthority(void) {
    return dusk::coop::forms::isStoryAuthorityWolf() ? 1 : 0;
}

int dusk_coop_checkNowWolfEyeUp(void) {
    if (!dusk::coop::isEnabled()) {
        daAlink_c* link = daAlink_getAlinkActorClass();
        return link != nullptr ? link->checkWolfEyeUp() : 0;
    }
    const dusk::coop::PlayerId id = dusk::coop::currentPlayer();
    if (id == 0) {
        daAlink_c* link = daAlink_getAlinkActorClass();
        return link != nullptr ? link->checkWolfEyeUp() : 0;
    }
    // Secondary: sidecar senses flag (proxy has no mWolfEyeUp).
    return dusk::coop::forms::sensesActiveForPlayer(id) ? 1 : 0;
}

int dusk_coop_trySetTransformStatus(u8 status) {
    if (!dusk::coop::isEnabled()) {
        return 0;
    }
    const dusk::coop::PlayerId id = dusk::coop::currentPlayer();
    const dusk::coop::PlayerForm form =
        (status == TF_STATUS_WOLF) ? dusk::coop::PlayerForm::Wolf : dusk::coop::PlayerForm::Human;
    dusk::coop::forms::writeTransformSaveIfAuthority(id, form);
    // Always handled under co-op so secondary never falls through to the global write.
    return 1;
}

u8 dusk_coop_getTransformStatusForQuery(void) {
    // GLOBAL_UNLOCK / stage-start reads stay on the original save (Player 0 authority).
    return g_dComIfG_gameInfo.info.getPlayer().getPlayerStatusA().getTransformStatus();
}

int dusk_coop_sensesActiveForCurrentView(void) {
    if (!dusk::coop::isEnabled()) {
        return dusk_coop_checkNowWolfEyeUp() != 0 ? 1 : 0;
    }
    return dusk::coop::forms::sensesActiveForView(dusk::coop::currentView()) ? 1 : 0;
}

}

#endif  // ENABLE_LOCAL_COOP && TARGET_PC
