# Gate A Evidence — Incompatible Post-Processing Effects

When `dusk::coop::render::isMultiViewActive()` and `incompatibleEffectsDisabled()` (default **true**), `mDoGph_Painter` skips effects that assume a single full-frame camera or that sample/write the entire framebuffer.

| Effect | Hook | Why incompatible |
|--------|------|------------------|
| Motion blur | `motionBlure` | Full-frame feedback; not scissored per view |
| Depth of field | `drawDepth2` | Full-frame depth filter / focus tied to one camera |
| Framebuffer capture | `retry_captue_frame` | Copies whole FB; breaks tiled isolation |
| Bloom | `getBloom()->draw` | Full-frame post |
| Color fade | `calcFade` (multi-view) | Full-frame overlay; deferred until last pass and skipped when disabled |
| Mirror mode copy | settings mirror path | Full-frame tex copy/flip |
| Screen-space particles | policy enum reserved | Any particle path that ignores viewport scissor |

## Re-enable policy

Flip `setIncompatibleEffectsDisabled(false)` only after per-effect viewport tests. Prefer re-enabling one effect at a time with 2-view and 4-view layouts.
