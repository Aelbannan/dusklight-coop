# Gate A Evidence — dScnPly_Draw Trace

Audited against local tree (co-op fork). Source: `src/d/d_s_play.cpp` (`dScnPly_Draw`), draw-list consumers in `src/m_Do/m_Do_graphic.cpp` (`mDoGph_Painter`), buffers in `src/d/d_drawlist.cpp`.

## Call order inside `dScnPly_Draw`

| Phase | Work | Runs per frame | Multi-view note |
|-------|------|----------------|-----------------|
| 1 | Collision move (`dComIfG_Ccsp()->Move`), BG clear-move | Once | Must stay once |
| 2 | Stage-change / wipe request | Once | Global UI |
| 3 | `dMdl_mng_c::reset` | Once | Prep |
| 4 | Vibration, `daSus_c::execute`, BG move, **particle calc 3D/2D**, `cCt_execCounter` | Once (if not paused) | **Simulation boundary** — Gate A hooks `noteSimulationTick` here |
| 5 | `fopDwIt` → `fpcM_Draw` for every draw-tagged process | **Once** | Populates J3D draw buffers; **do not replay** |
| 6 | Eye highlight, collision/BG debug draw, **attention `Draw`** | Once today | Attention should become per-view later |
| 7 | Gate A `drawViews` / `endFrame` | Once | Syncs sidecar viewports + frame hash |

## Draw-list consumption (`mDoGph_Painter`)

After scene draw prep, the painter (not `dScnPly_Draw`) rasters lists:

1. Copy2D / ortho setup (once)
2. **Per view (Gate A loop):** window viewport/scissor → shadow image → sky → BG opa/xlu → particles (with view mtx) → shadow → opa/xlu lists → (optional) motion blur / DoF / FB capture → …
3. **Once after last view:** 2D-screen list, bloom (disabled in multi-view), trimming/fade
4. **Once globally:** wipe, HUD 2D opa/xlu, cursor, imgui

`J3DDrawBuffer::draw()` is `const` and non-destructive, so the same packets can be submitted for each viewport.

## Architectural rule

**Never call `dScnPly_Draw` once per view.** That would double particle calc, double `fpcM_Draw` side effects, and advance `g_Counter` / attention incorrectly.

Correct split:

```text
Simulation + draw prep:     dScnPly_Draw          ×1
Draw-list rasterization:    mDoGph_Painter views  ×N
HUD / wipe / fade:          after last view       ×1
```
