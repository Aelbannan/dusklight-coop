# Gate G — One Enemy Adapter

**Status:** 🟡 PoC implemented (in-engine verification pending)

**Depends on:** Gate D (Proxy Player), Gate F (Combat Attribution)

## Deliverables

- [x] Exact parameter map for one fodder enemy (Armos / `E_AI`)
- [x] Safe deterministic clone placement
- [x] Nonpersistent set ID (`0xFFFF`)
- [x] No duplicate switch/event from clone
- [x] ALLDIE waits for clones and pending waves
- [x] Bounded heap/collision usage (per-room clone cap)

## Acceptance criteria

- Clone is created with `fopAcM_create` overload using `0xFFFF` set ID
- Clone does not inherit or set any unique progression switch
- Clone placement: valid ground, capsule clearance, no hazard, deterministic
- `daAlldie_c` does not trigger room-clear while clones or pending waves exist
- Maximum per-room clone cap respected
- Clone is not recursively cloned

## Chosen fodder: Armos (`E_AI` / `fpcNm_E_AI_e`)

Documented example enemy in `impl/common/github_source_index.md`. Ordinary statue fodder with a short, fully-audited parameter word.

### Parameter map (`fopAcM_GetParam`)

| Bits | Field | Vanilla use | Clone policy |
|------|-------|-------------|--------------|
| 0–7 | `field_0x5ba` | Home-radius factor; `home_distance = 100 * value` | **Keep** from source |
| 8–15 | *(unused in `Create`)* | Not read by Armos create/execute paths audited | **Keep** verbatim |
| 16–23 | `m_swbit` | Death switch; Create aborts if already on; death calls `dComIfGs_onSwitch` | **Sanitize → `0xFF` (none)** |
| 24–31 | *(unused in `Create`)* | Not read | **Keep** verbatim |

Sanitizer: `(params & ~0x00FF0000) | 0x00FF0000`.

Other create inputs (not in the param word):

| Input | Source | Clone policy |
|-------|--------|--------------|
| `setId` | stage table | Always `0xFFFF` (nonpersistent) |
| `argument` | stage name-info | Copy from source |
| `scale` | actor scale | Copy from source |
| `angle` | home angle + deterministic yaw offset | Computed by placer |
| `roomNo` | source room | Same room |

### Not auto-duplicated

Elites, bosses, puzzle/scripted enemies, and any proc name without a whitelist adapter. Bokoblin (`E_OC`) and others remain stubbed until their parameter audits land.

## Implementation notes (2026-07-20)

### Working

| Area | Detail |
|------|--------|
| Adapter registry | `EnemyAdapter` + `registerAdapter`; Armos registered in `enemy::init` |
| Spawn choke | `e_ai_class::Create` → `noteEligibleSource`; room scan in `enemy::tick` |
| Create wrapper | `fopAcM_create(proc, 0xFFFF, sanitizedParams, …)` on play-scene layer |
| Switch sanitize | Mid-byte forced to `0xFF`; clones never arm a progression switch |
| Placement | Deterministic offset ring around `home.pos`; ground check, slope, water depth, hazard ground-codes `{4,5,9,10}`, capsule roof + cardinal wall probes |
| Non-recursive | Reject `setId == 0xFFFF` sources; track clone pids; never re-augment clones |
| Room cap | `maxClonesPerRoom = 4` (Armos); stop before exceeding |
| ALLDIE | `coopRoomStillHasEnemies` waits on `enemy::roomClearBlocked()` (pending creates + pending waves) |
| Party gate | Clones only when snapshotted party ≥ 2; count mul from Gate H |
| Drop credit share | Gate H Strategy A (`gateEnemyDropCandidate`) |

### Stubbed / deferred

| Area | Detail |
|------|--------|
| Pending waves API | `pendingWaveCount()` / `g_pendingWaves` exists; no wave scheduler yet |
| Multi-enemy adapters | Only `E_AI`; Bokoblin et al. need their own parameter maps |
| Encounter budgets | Heap/collision byte budgets beyond per-room clone cap |
| Full count + HP + drops | Wired in Gate H (`coop_difficulty` / `coop_drops` + enemy hooks) |

## Evidence log

| Date | Evidence | Status |
|------|----------|--------|
| 2026-07-20 | Armos adapter + sanitize + `0xFFFF` create + placement + ALLDIE wait + room cap in `coop_enemy.*`, hooks in `d_a_e_ai.cpp` / `d_a_alldie.cpp` | Code complete; runtime test pending |

## How to test

1. Build with a normal PC build (PC).
2. **SP regression:** co-op disabled — Armos and ALLDIE behavior unchanged (no clone notes, no ALLDIE wait).
3. Join P1 (Press Start) in a room with a stage-placed Armos (`E_AI`, real set ID).
4. Confirm log: `enemy: clone requested … setId=0xFFFF` and params mid-byte `FF`.
5. Confirm clone stands on valid ground near the original; no spawn on void/lava/water.
6. Kill the **clone** first — room switch / ALLDIE must **not** fire if the original (or pending clone) remains.
7. Kill all Armos including clones — ALLDIE proceeds after the normal 65-frame timer.
8. Confirm at most **4** clones in the room; a clone never produces another clone.
9. Confirm original death still sets its stage switch; clone death does not.

## Sign-off

| Role | Name | Date |
|------|------|------|
| Gate keeper | | |
