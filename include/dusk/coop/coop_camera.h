#pragma once

#include "dusk/coop/coop_types.h"

namespace dusk::coop::camera {

void init();
void reset();
void tick();

// Gate B: create/schedule cameras 0..N-1 through the process manager.
// Does NOT manually execute camera processes — the fpc scheduler owns that.
bool ensureCameras(uint8_t count);
void destroyCamera(ViewId id);
void destroySecondaryCameras();

bool assignInputOwner(ViewId id, PlayerId owner);
bool assignTrackedPlayer(ViewId id, PlayerId player);
bool assignAttentionOwner(ViewId id, PlayerId owner);
bool assignWindow(ViewId id);

// Noncontiguous remove/rejoin must not shift other camera slots.
bool removeCameraSlot(ViewId id);
bool rejoinCameraSlot(ViewId id, PlayerId owner);

uint8_t activeCameraCount();
bool isCameraActive(ViewId id);

// True when this dCamera body must skip primary-only global destructor side effects.
bool isSecondaryCameraBody(const void* dCameraBody);

}  // namespace dusk::coop::camera
