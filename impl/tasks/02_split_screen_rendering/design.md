# Split-Screen Rendering — Design

## Corrected render architecture

Do not call `dScnPly_Draw()` once per view — it advances systems beyond raster submission. Risk: double particle calculations, double draw-process callbacks, double attention drawing, mutation of actor state during second pass.

## Two explicit render preparation classes

```cpp
enum class RenderPreparationClass {
    CameraIndependent,
    CameraDependentPure,
    CameraDependentMutating,
};
```

Target architecture:

```text
Simulation update:                    Once
Global draw preparation:              Once (camera-independent work)
Per-view preparation:                 Once per view (audited camera-dependent pure work)
Per-view draw-list consumption:       Once per view
Global UI/fade:                       Once
```

`CameraDependentMutating` draw code must be refactored so mutation occurs in simulation or camera-independent preparation.

## Render-pass context

Must save/restore: current window, view, viewport, graphics port, projection/view matrices, aspect ratio, active camera, active attention owner, frame-interpolation namespace.

## Viewports and scissors

```cpp
void configureHorizontalSplit(float width, float height) {
    const float half = height * 0.5f;
    setViewportAndScissor(0, 0.0f, 0.0f, width, half);
    setViewportAndScissor(1, 0.0f, half, width, half);
}
```

Per-view aspect: `aspect = viewportWidth / viewportHeight`. Keep vertical FOV constant.

## Clear policy

Per view: scissor to view rectangle, clear depth within rectangle, clear color only when rendering to isolated offscreen target. Never issue whole-frame clear after View 0 composited.

## Rendering backends

**Direct tiled rendering** (prototype): viewport/scissor into main target. Lowest memory.

**Per-view offscreen rendering** (escalation): eight color/depth targets, composited into grid. Best effect isolation.

Support both; default to direct tiled first.

## Post-processing policy (initial)

Disable: full-frame motion blur, whole-frame damage flash, screen-space distortion, lens effects tied to one camera, effects sampling full-screen depth/color without viewport bounds. Re-enable one at a time with per-view tests.

## Eight-view rendering cost model

Eight viewports ≠ 8× pixel fill (tiled 4×2 sum ≈ one output frame). However these may approach 8×:

- CPU draw traversal / camera-dependent actor preparation
- Draw-call submission
- Vertex processing / frustum culling
- Camera-facing billboards
- Shadow-map rendering if per-view
- Per-view post-effects

**Optimization order:**
1. Reuse camera-independent simulation and draw preparation
2. Build one union-visible actor set for all views
3. Eight-bit visibility mask per renderable actor
4. Skip in views where mask bit is clear
5. Batch repeated model/material work
6. Share shadows/reflections unless per-view essential
7. Dynamic per-view render resolution
8. UI at output resolution

## Visibility mask

```cpp
using ViewMask = uint16_t;
constexpr ViewMask viewBit(ViewId view) { return ViewMask{1} << view; }

struct RenderVisibility {
    ViewMask visibleViews = 0;
};
```

## Per-view resolution

```cpp
struct ViewRenderResolution {
    uint16_t width, height;
    float scale;
};
```

At 3840×2160 with 4×2 grid: each native cell = 960×1080. At 0.75 scale: 720×810 composited into 960×1080 cell. One shared scale initially.
