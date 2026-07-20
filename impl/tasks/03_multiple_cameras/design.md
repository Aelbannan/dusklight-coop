# Multiple Cameras — Design

## Let the normal process scheduler execute cameras

Create additional camera processes through the camera manager and let the existing process loop execute them once. Do **not** manually call `fpcM_Execute(camera)` — that risks double execution.

## Route construction before process creation

`dCamera_c` derives camera ID, player mapping, controller mapping, and window mapping during initialization. Flow:

1. Allocate and initialize the sidecar route
2. Make co-op-aware mapping accessors visible
3. Create the camera process
4. Capture the returned process ID
5. Resolve and store its `camera_class*` after creation

## Secondary-camera destructor guard

Current camera destruction writes turn-restart camera data and clears global stop status. Guard:

```cpp
dCamera_c::~dCamera_c() {
#if TARGET_PC
    if (dusk::coop::isSecondaryCamera(this)) {
        releaseSecondaryCameraResourcesOnly();
        return;  // skip global side effects only; normal destruction order preserved
    }
#endif
    originalPrimaryCameraDestructorSideEffects();
}
```

Do not early-return before C++ base/member destruction — implement guard around the global side-effect block.

## Input owner

For players above legacy port 3, add a camera input provider:

```cpp
struct CameraInput { float lookX, lookY; bool center, lock; };
CameraInput getCameraInput(PlayerId player);
```

## Attention owner

Camera code queries global attention. Resolve using the camera's `inputOwner` or store a direct non-owning pointer to that player's attention object. Fallback: Player 0 attention.

## Eight-camera requirements

- Expand PC camera-manager registry to eight
- Bounds checks on `fopCamM_Create`
- Destroy/unregister helper clears stale process IDs
- Every ID routed through PC camera sidecar
- Prevent secondary destructor side effects for Cameras 1-7
- One input owner, player target, attention owner, window, interpolation namespace per camera
- Noncontiguous active camera sets after leave/rejoin
- Camera process ID must not be confused with camera index
