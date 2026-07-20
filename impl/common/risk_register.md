# Risk Register

## Highest-risk areas

### Full second `daAlink_c`
- **Risk:** Hidden singleton assumptions; static locals; global equipment/action state.
- **Mitigation:** Proxy actor first; audit in functional groups; preserve exact single-player path.

### Independent form and Midna assumptions
- **Risk:** Global save transform status; no-argument global wolf checks; standalone Midna bound to global Link; senses/screen effects leaking across views.
- **Mitigation:** Per-player form sidecar; context-aware wrappers; per-Link embedded rider models; per-view senses rendering.

### Multiple Eponas
- **Risk:** Global horse pointer/singleton; Link horse code reading wrong horse; horse code reading global Player 0; one global restart position; eight-horse resource budget; scripted events assuming one Epona.
- **Mitigation:** Player-owned horse registry; `mRideAcKeep` as mounted authority; owner-aware context; sidecar restart data; profiling; explicit event adapters.

### Rendering twice
- **Risk:** Running simulation-adjacent work twice; post-processing leakage; camera-dependent caches.
- **Mitigation:** Split submission from view consumption; key caches by view; disable complex post-processing initially.

### Enemy cloning
- **Risk:** Duplicate progression switches; unique child actors; path/event conflicts; room-clear bugs.
- **Mitigation:** Strict whitelist; per-enemy parameter sanitizer; no persistent set ID; encounter-level completion.

### Combat attribution
- **Risk:** Enemy reads Player 0 cut type after Player 2 hits it; projectile has no owner; wrong rumble/resource deduction.
- **Mitigation:** Combat registry; explicit owner IDs; normalized hit events; avoid relying only on active context.

### Save compatibility
- **Risk:** Expanded original save corrupts old saves; co-op data mismatches original progression.
- **Mitigation:** Companion file; original-save CRC; versioned migrations; sanitization on load.

## Medium-risk areas
- Multiple attention-manager resource ownership.
- Frame interpolation with multiple cameras.
- Full-screen UI transitions.
- Item-get cinematics.
- Shop customer identity.
- Magic Armor rupee ownership.
- Bottle item representation.
- Dynamic controller persistence.
- Large-enemy encounter performance.
