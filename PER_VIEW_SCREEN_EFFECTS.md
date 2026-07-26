# Per-View Screen Effects

## Goal

Re-enable the non-HUD effects currently suppressed or altered by `coopMultiView`, without changing the single-view path:

- motion blur
- framebuffer-dependent water refraction/reflections
- depth of field, including Dusk DoF
- bloom, Classic and Dusk
- screen-space/projection particles
- screen-space display lists and fades
- mirror mode and other final-screen effects where meaningful

This is an implementation plan, not an enable-all-guards change. The co-op renderer renders each view full-frame, captures it, and only then composites the captures into the final N-up grid. Effects must therefore run either inside each full-frame view pass or after the grid is composited.

## Current audit

Primary code is in `src/m_Do/m_Do_graphic.cpp` and `src/dusk/coop/coop_render.cpp`.

| Feature | Current multi-view status | Root issue |
|---|---|---|
| Motion blur | Skipped around `motionBlure()` | Reads a single global previous-frame texture and blur matrix |
| DoF | Skipped around `drawDepth2()` | Captures color/depth into global textures and computes focus from player/camera 0 |
| Water refraction | Draw lists still execute, but their input is broken | `drawDepth2()`'s EFB capture is skipped before invisible water lists |
| Retry framebuffer capture | Skipped at several call sites | `mFrameBufferTex` is never updated for the active view |
| Bloom | Skipped in the fullscreen block | Reads global framebuffer state and is positioned as a single full-screen pass |
| `XluList2DScreen` | Skipped in multi-view | Fullscreen/screen-space list is only consumed in the single-view block |
| 2D game particles | Skipped in the per-view fullscreen block | Uses full-frame orthographic coordinates and is not replayed per view |
| 3D-last list | Already replayed per view before capture | The normal later invocation is suppressed to avoid duplication |
| Trimming | Already replayed per view | Required before capture |
| Fade | Normal multi-view call is skipped | Can be applied uniformly after grid presentation |
| HUD | Skipped during raw view rendering, then redrawn per view after grid | Already intentional and correct |
| Mirror mode | Final full-frame copy is skipped | Would flip the already-composited grid unless explicitly supported |
| Final 2D lists/menu particles | Run after grid as global output | Must remain global for menus, or be split into per-view gameplay/UI lists |

The important ordering bug is:

```text
main 3D lists
  -> drawDepth2() captures EFB color/depth for DoF and water
  -> invisible water lists sample that capture
  -> later framebuffer retry capture
```

Multi-view currently removes the first capture, so water samples stale or uninitialized `mFrameBufferTexObj` data.

## Design rules

1. **Preserve the single-view path.** All new behavior is under `TARGET_PC` and multi-view checks; vanilla Wii/GC behavior remains unchanged.
2. **Use the existing full-frame-per-view model.** Do not apply a pane-sized viewport during world rendering. Each camera renders into the full EFB, then its processed result is captured and tiled.
3. **Classify effects by timing.**
   - View-dependent effects run before `captureView(view)`.
   - Uniform output effects run after `presentMultiViewGrid()`.
   - Global menus remain after the grid and are not duplicated into every world pass.
4. **Never share temporal state across views.** Previous-frame textures, blur matrices, blur rates, focus values, and view-specific effect flags need per-view ownership.
5. **Do not use `m_fullFrameBufferTex` as a working effect texture.** It is a presentation capture slot. Add separate effect resources or explicit input/output bindings.
6. **Every EFB copy must identify its view and purpose.** Water/DoF working copies, motion history, and presentation captures must not alias accidentally.
7. **Do not re-run simulation or mutating preparation.** Effects may replay raster work per view, but must not advance particles, actors, fades, or global timers more than once per frame.

## Proposed render flow

For each active view:

```text
ScopedWorldDrawPass(view)
  configure current window/view/viewport/context
  bind pane-aspect camera matrices

  render sky/background/opaque/xlu/3D particle lists

  motion blur using history[view]              [per-view temporal input]
  capture current EFB color+depth              [before water]
  drawDepth2 for this camera/player            [DoF + water input]
  draw invisible water/refraction lists
  draw projection/screen/filter particle lists
  capture retry/current color                  [for later effects/history]
  draw IndScreen and 3D-last lists
  draw per-view screen-space game particles
  draw per-view XluList2DScreen effects
  bloom for this view
  capture processed view into presentation slot
  capture next history image at the chosen vanilla-equivalent point

presentMultiViewGrid()
  apply global fade/wipe if the effect is uniform
  draw per-view HUD
  draw global menus/final 2D lists
```

The exact placement of motion blur, history capture, 3D-last, and bloom must be validated against the vanilla single-view order. In particular, do not capture a history image after HUD or menus unless vanilla does so.

## Resource architecture

Add a render/effects resource layer, preferably in:

- `include/dusk/coop/coop_render.h` or a new `include/dusk/coop/coop_effects.h`
- `src/dusk/coop/coop_render.cpp` or a new `src/dusk/coop/coop_effects.cpp`

A resource should contain at least:

```cpp
struct ViewEffectResources {
    ResTIMG* historyTimg = nullptr;
    void* historyTex = nullptr;
    TGXTexObj historyObj{};
    bool historyValid = false;

    ResTIMG* colorTimg = nullptr;
    void* colorTex = nullptr;
    TGXTexObj colorObj{};

    ResTIMG* depthTimg = nullptr;
    void* depthTex = nullptr;
    TGXTexObj depthObj{};
};
```

The actual color/depth scratch resources may be shared sequentially if ownership is proved, but separate per-view resources are the safer initial implementation and prevent view leakage while debugging.

### Allocation/lifetime

- Allocate for active views in `beginMultiViewCapture()` or a dedicated `ensureEffectResources()` call before the view loop.
- Match the current Aurora render size and copy format.
- Keep destination pointers stable while a scene is active; Aurora keys copy textures by destination pointer, dimensions, and format.
- Release on `render::reset()` and resize/reconfiguration.
- Invalidate history on first allocation, scene transition, camera reassignment, player join/leave, and resolution change.
- Consider calling Aurora's `GXDestroyCopyTex()` before releasing/reusing a destination pointer; the Aurora copy-texture cache otherwise retains resources.
- Keep effect resources separate from `s_captureSlots`; presentation slots are consumed by `presentMultiViewGrid()` and must not double as temporal history.

### First-frame behavior

If `historyValid[view]` is false, skip motion blur for that view and capture its first valid history image after the view has rendered. This prevents garbage or another player's image appearing during joins and transitions.

## Feature implementation plan

### Phase 0 — Instrumentation and contracts

1. Add named debug groups/log counters for:
   - per-view pre-water capture
   - DoF color/depth capture
   - retry framebuffer capture
   - motion history load/store
   - bloom input/output
   - per-view screen-space lists
   - final composite effects
2. Add assertions that the active `ScopedContext` view matches the effect resource view.
3. Add a per-view render-stage enum or comments documenting the intended ordering.
4. Add a debug mode that fills each view's EFB with a unique color before processing, making cross-view sampling obvious.
5. Keep `gate_A_incompatible_effects.md` accurate as each feature moves from skipped to validated.

### Phase 1 — Per-view framebuffer capture and water

This is the prerequisite for DoF and water.

1. Extract the capture portion of `drawDepth2()` into a helper accepting:
   - `ViewId`
   - current `camera_process_class*`
   - current `view_port_class*`
   - destination color/depth resources
2. Capture the active full-frame EFB color and Z before `drawOpaListInvisible()` / `drawXluListInvisible()`.
3. Bind the active view's captured color texture to the texture map expected by water materials.
4. Keep the later `retry_captue_frame()` semantics as a separate per-view capture; do not blindly replace all captures with `captureView()`, because `captureView()` targets presentation textures and water/DoF use different formats and dimensions.
5. Confirm that Aurora copy commands are ordered correctly with `GXPixModeSync()` / texture invalidation before dependent draws.
6. Test water with two views whose cameras are on opposite sides of a water plane. Each view must sample its own scene, not the other view or the previous frame.

### Phase 2 — DoF and Dusk DoF

#### View-dependent inputs

Refactor `drawDepth2()` so it does not assume the primary camera/player:

- Replace `daPy_getLinkPlayerActorClass()` with the active view's player.
- Replace `dComIfGp_getCamera(0)` with the active camera passed from `mDoGph_Painter`.
- Replace `dComIfGp_getPlayerCameraID(0)` with the current window/camera ID.
- Replace `dCam_getBody()` with the camera body associated with the active view.
- Resolve lock-on/attention data per view, or explicitly use a view-local focus target when the global attention system cannot provide one.
- Use the active `ScopedContext` for form/player-dependent queries.

#### State isolation

`drawDepth2()` mutates shared state such as:

- `g_env_light.field_0x1264`
- `g_env_light.mDemoAttentionPoint`
- `mFrameBufferTexObj`
- `mZbufferTexObj`

Either make focus state per view, or save/restore shared values around each view and ensure no later view observes the previous view's transient values.

#### Effect processing

1. Run pre-water color/depth capture for the view.
2. Run DoF using that view's depth/color resources.
3. Continue the water/invisible lists using the resulting color texture.
4. For Dusk DoF, audit `drawDepth_blurTex()`'s offscreen passes and ensure every temporary copy is scoped to the current view and restored with balanced `GXCreateFrameBuffer()` / `GXRestoreFrameBuffer()` calls.
5. Capture the processed view only after DoF and all desired per-view effects.

Start with Classic DoF, then Dusk DoF. Validate single-view output after every refactor.

### Phase 3 — Per-view retry captures

Replace the current multi-view guards around `retry_captue_frame()` with a view-aware helper rather than calling the legacy global function directly.

Required call sites include:

- the normal post-particle capture
- the F_SP124 special capture
- the water-in/D_MN08 capture used before bloom
- the debug darkworld capture, if debug rendering is supported in co-op

Each call must:

- copy the active view's current EFB into the correct working texture
- use the active view's viewport/scissor and current render dimensions
- bind the resulting texture before dependent draws
- avoid overwriting another view's history or presentation slot

### Phase 4 — Per-view bloom

Bloom can run while each view still owns a full-frame EFB. It should not run on the final N-up grid unless a deliberately global bloom is desired.

1. Move bloom into the per-view pass after the desired world/screen-space content and before `captureView(view)`.
2. Refactor `bloom_c::draw()` / `draw2()` to accept an explicit input texture and effect resource context instead of always using `getFrameBufferTexObj()` / global scratch textures.
3. Ensure the current EFB is captured into the bloom input after water and before bloom.
4. Keep intermediate textures and offscreen framebuffer passes isolated or prove they are safe for sequential reuse.
5. Balance every `GXCreateFrameBuffer()` with `GXRestoreFrameBuffer()` before the next view begins.
6. Validate both Classic and Dusk bloom, with different bright content in each view.
7. If desired, add a separate global post-composite bloom later; do not confuse that with correct per-view bloom.

### Phase 5 — Per-view motion blur and history

Motion blur is the most temporal-sensitive feature.

1. Add one persistent history color resource per view.
2. Add per-view validity, blur rate, and blur matrix state. If the game only exposes global `mBlureFlag`, `mBlureRate`, or `mBlureMtx`, snapshot them into the active view at the camera/player update point or add a co-op bridge.
3. Change `motionBlure()` to accept a view/resource context and load `history[view]`.
4. Capture the current view into `history[view]` at the vanilla-equivalent point for use on the next frame.
5. Ensure view 0 never reads view 1's history when the loop advances within the same frame.
6. Invalidate histories on joins, leaves, scene transitions, camera changes, resize, and pause/event transitions as appropriate.
7. Confirm that the history copy occurs before grid presentation and does not include HUD/global menus.
8. Test stationary views, camera pans, independent player movement, 2-view and 4-view layouts, and a view joining mid-frame.

### Phase 6 — Screen-space particles and lists

#### Already per-view

The following are already consumed inside the view loop and should be regression-tested rather than duplicated:

- fog/normal priority particle lists
- projection particles
- screen particles drawn inside the world pass
- invisible/filter lists
- Z-translucent list
- `dOpaList3Dlast`, which is deliberately moved before per-view capture

Audit these for hidden camera-0/player-0 lookups and mutation during draw.

#### Must be moved into each view pass

- `dComIfGp_particle_draw2Dgame()` from the skipped fullscreen block
- `dComIfGd_drawXluList2DScreen()` where the effect is world/camera-dependent
- any gameplay screen-space effect whose state differs by player or camera

Use the full-frame-per-view ortho during the view pass, then capture the result. Do not use final grid-cell coordinates until after `presentMultiViewGrid()`.

#### Must remain global or be explicitly split

The final post-composite lists include menus and global UI:

- `particle_draw2Dback`
- `particle_draw2DmenuBack`
- `draw2DOpa`
- `drawItem3D`
- `draw2DOpaTop`
- `draw2DXlu`
- pause/menu foreground particles
- `particle_draw2DmenuFore`

Keep these global unless they are divided into gameplay-per-view and menu-global lists. Do not render a global menu once per view.

### Phase 7 — Fade, wipe, and mirror mode

#### Fade

Color fade is uniform global state. Apply it once after `presentMultiViewGrid()` and before per-view HUD/global UI, or render it once into every captured view if independent fades are eventually required. Preserve the special F_SP127 and fade-bit conditions.

#### Wipe

Keep wipe transitions global unless a stage transition requires per-view camera-specific wipes. Verify that the wipe is drawn after grid presentation and does not get overwritten by HUD ordering.

#### Mirror mode

Implement only after the normal grid/effect pipeline is stable:

- capture/present the final grid
- flip the composed grid once if global mirror mode is intended, or
- flip each captured view before grid presentation if each pane should be mirrored independently

Do not use the current single-view full-frame copy unchanged; it would either flip the wrong buffer or interact badly with the grid present.

## API and file changes

Likely touch points:

| File | Work |
|---|---|
| `src/m_Do/m_Do_graphic.cpp` | Reorder Painter stages, add view-aware captures, refactor DoF/motion/bloom entry points, per-view screen-space draws |
| `include/m_Do/m_Do_graphic.h` | Expose/refactor effect state only if necessary |
| `src/dusk/coop/coop_render.cpp` | Allocate/release effect resources, view capture helpers, lifecycle/reset/resize integration |
| `include/dusk/coop/coop_render.h` | Public per-view capture/history/effect APIs |
| `src/dusk/coop/coop_context.cpp` / headers | Use active view/player context where existing helpers are insufficient |
| `src/d/d_camera.cpp` / camera headers | Provide current-view camera body/focus/attention access if DoF needs it |
| `src/d/d_com_inf_game.cpp` | Keep water invisible-list policy while binding view-specific resources |
| `src/d/d_drawlist.cpp` / headers | Only if screen-space lists need explicit per-view replay or resource binding |
| `extern/aurora/lib/dolphin/gx/GXFrameBuffer.cpp` | Only if copy-resource destruction/resize invalidation is required; prefer game-side APIs first |
| `impl/gates/evidence/gate_A_incompatible_effects.md` | Update effect status and evidence after validation |

A new `coop_effects` module is recommended if `coop_render.cpp` becomes too large. It should own effect resources and expose narrow operations such as:

```cpp
void ensureViewEffectResources(ViewId view);
void invalidateViewEffectHistory(ViewId view);
void captureViewColorDepth(ViewId view, CapturePurpose purpose);
void bindViewHistory(ViewId view);
void captureViewHistory(ViewId view);
void applyViewDepthOfField(ViewId view, camera_process_class* camera,
                           view_port_class* viewport);
void applyViewBloom(ViewId view);
```

## Verification matrix

### Functional

- Single-view, co-op disabled: bloom/DoF/motion blur/water output matches baseline.
- Two views with different camera positions over water: no cross-view reflection/refraction.
- Two views with different lock-on targets/distances: DoF focus differs correctly.
- Two views with different bright objects: bloom stays inside the correct captured pane.
- Two views moving independently: motion blur history is independent.
- Four and eight views: no stale textures, texture cache failures, or pane bleed.
- Toggle effects at runtime.
- Join/leave a player while effects are active.
- Transition between single-view event camera and multi-view gameplay.
- Resize/window scale changes while resources are live.
- Wolf/human and underwater state differ between players.
- Fade/wipe/mirror mode behavior is intentional and does not cover or erase the grid unexpectedly.

### Isolation and ordering

- Fill each view's source EFB with a unique diagnostic color; every effect must preserve the correct color identity.
- Enable only one effect at a time, then combine:
  1. water capture
  2. DoF
  3. bloom
  4. screen-space particles
  5. motion blur
  6. fade/mirror
- Verify `GXCreateFrameBuffer()`/`GXRestoreFrameBuffer()` balance with debug assertions.
- Verify every `GXCopyTex` destination pointer remains stable and maps to the intended view/purpose.
- Verify `ScopedContext` is non-empty for all effect code using `currentPlayer()`/`currentView()`.
- Confirm draw preparation and simulation counters remain one-per-frame.

### Performance

Measure 1, 2, 4, and 8 views with each effect separately and combined. Expect the largest costs from:

- per-view DoF color/depth copies
- bloom pyramid passes
- motion-history copies
- repeated screen-space draw traversal

Add effect quality/resolution controls if the combined 4–8 view cost is excessive. Do not silently fall back to a shared history texture.

## Recommended implementation order

1. Instrumentation and resource lifecycle.
2. Per-view pre-water color/depth capture; fix water.
3. Per-view retry captures.
4. Classic DoF with view-aware camera/focus state.
5. Dusk DoF.
6. Per-view screen-space gameplay particles and screen list.
7. Per-view Classic/Dusk bloom.
8. Per-view motion history and motion blur.
9. Global fade/wipe after grid.
10. Mirror mode and final menu/UI classification.
11. Combined performance tuning and gate evidence updates.

Do not make `incompatibleEffectsDisabled(false)` the implementation. Remove each guard only after that effect has an explicit input resource, ordering contract, and 2-/4-view validation.
