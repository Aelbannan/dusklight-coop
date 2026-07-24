#pragma once

#include "dusk/coop/coop_types.h"

class daAlink_c;
class fopAc_ac_c;

namespace dusk::coop::forms {

enum class WolfQueryKind : uint8_t {
    CurrentLink,
    SpecificActor,
    ViewOwner,
    EventParticipant,
    StoryAuthority,
    GlobalUnlock,
    UnsafeUnresolved,
};

enum class FormRule : uint8_t {
    Either,
    ForceHuman,
    ForceWolf,
    NoTransformation,
};

struct StageFormPolicy {
    FormRule rule = FormRule::Either;
    // ForceWolf / story force: which player is the event participant (-1 = none).
    int8_t eventParticipant = -1;
    // When true, NoTransformation / ForceHuman apply to every joined player.
    bool forbidAll = false;
};

void init();
void reset();
void tick();

PlayerFormState& state(PlayerId id);
PlayerMidnaRuntime& midna(PlayerId id);

bool isWolf(PlayerId id);
bool isCurrentContextPlayerWolf();
// Standalone Midna / story demos use the configured story-authority player.
bool isStoryAuthorityWolf();
bool isTransformUnlocked();

bool setDesiredForm(PlayerId id, PlayerForm form);
bool beginTransform(PlayerId id, PlayerForm form);
void onTransformComplete(PlayerId id);

// Update indexed form state and mirror only the designated story authority to vanilla save data.
void writeTransformSaveIfAuthority(PlayerId id, PlayerForm form);

// Called from changeWolf / changeHuman after models swap.
void notifyLinkFormChanged(daAlink_c* link, PlayerForm form);

PlayerId playerIdForActor(const fopAc_ac_c* actor);
PlayerId playerIdForLink(const daAlink_c* link);

bool sensesActiveForView(ViewId view);
bool sensesActiveForPlayer(PlayerId id);
void setSenses(PlayerId id, bool active);
// Sync senses from every live Link actor.
void syncSensesFromLinks();

FormRule stageRule();
const StageFormPolicy& stagePolicy();
void setStagePolicy(StageFormPolicy policy);
void clearStagePolicy();
// Returns false when the stage forbids the requested form for this player.
bool allowsForm(PlayerId id, PlayerForm form);
// Apply a forced form to the indexed event participant.
void applyForcedFormDemo(PlayerId participant, PlayerForm form);
void restoreFormsAfterForcedDemo();

}  // namespace dusk::coop::forms

#include "dusk/coop/coop_forms_bridge.h"
