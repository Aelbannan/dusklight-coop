# Adversarial review — M0 network layer

Reviewer: **deepseek/deepseek-v4-flash-0731** (adversarial pass)
Scope: M0 of the networked co-op, branch `net-coop`, commits
`b1ca17fabd..ee4e5e088e` (six commits).
Spec contract: `docs/design/mod-coop/00-network.md` §1–§5, `implementation-plan.md`
Rev 3 §5 (M0).
Method: read design + code, **built and ran** the selftest and the full game on
the existing macOS CMake+Ninja preset, ran three standalone load experiments
(SPSC ring stress, wire-determinism, transport under flood), and byte-compared
the vendored ENet against the official v1.3.18 tarball.

---

## VERDICT

**M0 meets its acceptance criteria as implemented.** No blockers found. The
handshake is genuinely exercised over real ENet sockets, the build is clean,
and the wire protocol matches the §1–§5 contract. The findings that matter are
forward-compatibility hazards for M1 (WorldInit growth, reliable-event drops,
uninitialized wire bytes, peer-slot reuse), none of which fail M0's own scope.

| Acceptance criterion (plan §5 M0) | Status | Evidence |
|---|---|---|
| mod-free build runs | ✅ | Full `dusklight` target builds and links `libenet.a` (`CMakeLists.txt:349-362`); `dusk::net::*` symbols present in the binary (`nm`). |
| netcode skeleton compiles | ✅ | Clean rebuild of the 5 net TUs produced **zero warnings**; selftest target builds clean. |
| LAN host/client handshake works with no game code attached | ✅ | `./dusk_net_selftest` → `PASS`, exit 0; **15/15 repeat runs**; `leaks --atExit` → **0 leaked bytes**. |
| GAME_SERVICE_MAJOR hygiene | ✅ | Upstream main has `1u`; fork `2u`; both SDK trait (`sdk/include/mods/svc/game.h:22`) and game module (`src/dusk/mods/svc/game.cpp:9,16`) use the macro — nothing else hardcodes the epoch. |

### What the selftest actually exercises (verified)

- **Real sockets, two ENet hosts**: the demo binds a host `Session` to an
  ephemeral UDP port and connects separate client `Session`s via
  `enet_host_connect` on 127.0.0.1 — each side has its **own ENetHost, own
  socket thread, own SPSC rings**. This is inter-host UDP over loopback, not an
  in-host loopback. The transport log (`peer N connected/disconnected`) and
  session log (`JoinRequest → JoinAccept + WorldInit`, both reject paths,
  `PlayerLeave` relay, host `SessionEnd`) confirm the full handshake round-trip.
- **What it does NOT exercise**: packet loss/retransmit (loopback is lossless),
  simultaneous peers (A/B/C/D are sequential), ring overflow (never stressed —
  the demo's largest message is 324 B), the unreliable channel (zero snapshot
  traffic crosses the wire; `ChannelFor` is unit-tested only), packets near
  `kMaxMessageSize`, the ENet connect-timeout path, Windows, peer-slot
  reuse after disconnect, and any drop/recovery path.

---

## RANKED FINDINGS

### BLOCKER

None.

### MAJOR

**M1. WorldInit cannot grow to carry the full-state snapshot the code comments and the plan promise, and the exact-size validator forbids growth.**

- Claim: `protocol.h:222-224` documents "M1 appends player/enemy state
  snapshots as count-prefixed sections after the roster; M0 always carries
  count 0", but `DeserializeMessage` rejects any packet whose payload size
  differs from `WireSize(type)` exactly (`src/dusk/net/protocol.cpp:250` —
  `if (payloadSize != WireSize(type) || r.remaining() < payloadSize) return false;`).
  The moment M1 appends one byte, every client rejects the packet.
- Numerically it cannot fit anyway: M1's D4 raw-matrix pose ≈ 2.7 KB/player
  (`implementation-plan.md` D4; `00-network.md` §5), so 8 players × 2.75 KB +
  40 enemies × ~60 B ≈ **24 KB > `kMaxMessageSize` (4096, `protocol.h:57`)**.
  `WorldInitMsg`'s `playerStateCount`/`enemyStateCount` fields (`protocol.h:228-229`)
  are pure decoration in M0.
- Why it matters: mid-game join in M1 is specced (§4) to receive a full state
  snapshot via WorldInit. The current design boxes that path in.
- Fix (recommend): **drop the sections from WorldInit entirely** — keep it a
  fixed 312 B (stage + roster), and deliver current pose via the existing
  per-frame `PlayerState`/`EnemySnapshot` stream (first frame after join arrives
  within 16 ms). If a guaranteed-snapshot-on-join is wanted, send it as a burst
  of ordinary per-player `PlayerState` messages after `WorldInit` — no protocol
  change needed. Update the `protocol.h` comment; either way `DeserializeMessage`
  should stay exact-size (its strictness is good).

**M2. Reliable-channel messages share the drop-on-full ring path with snapshots; a dropped reliable event is never retried.**

- Claim: one inbox + one outbox ring (`transport.h:177-178`, `InboundPacket`/
  `OutboundPacket` at `transport.h:101-117`); overflow returns false and drops
  with only a counter + log (`transport.cpp:127` outbox, `:188` inbox). ENet
  reliability only covers socket→socket (`enet_peer_send`); a reliable message
  that dies in the ring is lost forever — no retry, no app-level ack.
- M1 numbers: ~7 remote players + 40 enemies at 60 Hz ≈ 47 packets/frame
  inbound at the host; ring headroom is 128 slots ≈ 2.7 frames. A game-thread
  hitch (stage load, debugger pause) or socket-thread stall fills the inbox and
  drops `CombatIntent`/`EnemyEvent`/`PlayerEvent`/`PlayerLeave` silently — a
  correctness bug, not a cosmetic one.
- Fix: split reliable and unreliable traffic at the session layer (separate
  rings or a per-(peer, channel) pending slot), and make reliable overflow an
  explicit, visible failure (log + session-level retry or disconnect), not a
  silent drop. Keep snapshots drop-tolerant.

**M3. Uninitialized `reserved`/unset fields are serialized onto the wire (empirically confirmed).**

- Claim: the payload structs' `= {}` default member initializers
  (`protocol.h`, e.g. `JoinRequestMsg::reserved[3]`) are **inert inside
  `PayloadUnion`**, whose default ctor is `PayloadUnion() {}` and never
  value-initializes members (`protocol.h:270-276`). Session code sets only the
  meaningful fields (`session.cpp:294-299`, `:349-352`, `Stop()` paths,
  `HandleConnect` JoinRequest at `:163-168`).
- Evidence (standalone harness, `/tmp/det_test.cpp`, built against the repo's
  `protocol.cpp`): serializing a JoinRequest with only version/slot/name set
  emits `01 00 28 00 01 00 00 00 ff d7 c6 fa 54 65 73 74...` — bytes 7–9
  (`d7 c6 fa`) are stale stack garbage in `reserved[3]`. Identical logical
  messages produce different wire bytes across processes.
- Why it matters: nondeterministic wire bytes defeat golden-vector tests, any
  future wire hashing/checksum, and any later use of those fields (which would
  then be a silent protocol change). Today the receiver ignores them, so this
  is hygiene — but it is the kind of thing that bites at the worst time.
- Fix: `PayloadUnion payload = {};` at every construction site (`session.cpp`
  ×5, `selftest_main.cpp` `MakeMessage`), or zero the union in its default ctor.

**M4. Stale outbox entries can be delivered to a reused peer slot.**

- Claim: the game thread enqueues `(peerIndex, payload)` into the outbox ring;
  slots are assigned/`nullptr`-released only on the socket thread
  (`transport.cpp:240-254`). Sequence: peer N disconnects → slot freed → new
  peer connects → `AssignPeerSlot` hands out N again → `DrainOutbox`
  (`transport.cpp:212-232`) sends the stale packet to the *new* connection.
- Why it matters: M0's demo never frees + reuses a slot, so it is untested.
  M4 explicitly plans "slot kept for rejoin" — under reconnect churn a stale
  `JoinAccept`/`SessionEnd`/`PlayerState` addressed to the old peer can be
  misdelivered to the new one. The session's `peerToPlayer_` map
  (`session.cpp:280`, `session.h`) partially guards session-level messages but
  cannot see packets already sitting in the ring.
- Fix: add a per-slot connection **generation counter**, stamp outbox entries
  at enqueue, and drop on mismatch at drain. Cheap and makes misdelivery
  impossible.

**M5. Semantic fields are not validated on receive: `jointCount` and `assignedPlayerId`.**

- Claim: `DeserializePlayerState` reads `jointCount` but never checks it
  (`protocol.cpp` PlayerState path); a packet with `jointCount = 255` passes
  the parser (the fixed 40-joint copy is safe, but the value is trusted).
  M1's apply code that indexes `joints[jointCount]` will read out of bounds.
  Similarly `OnJoinAccept` stores `assignedPlayerId` unchecked
  (`session.cpp:301-306`); `selfId_` can become 0xFF or 8+.
- Fix: clamp/reject `jointCount > kMaxJoints` at parse; validate
  `assignedPlayerId < kMaxLocalPlayers` (and optionally that the roster marks
  that slot present) before `selfId_ = ...`.

**M6. `DrainOutbox` placement is fragile — drain depends on inbound gaps ≥ 8 ms.**

- Claim: `DrainOutbox()` runs only after the inner
  `while (enet_host_service(...) > 0)` loop exits (`transport.cpp:145-201`),
  i.e. when ENet returns 0 (its 8 ms socket wait fully expires) or negative.
  Under a perfectly continuous inbound stream, `enet_host_service` can return 1
  indefinitely and the drain starves.
- Empirical result — **not reproduced**: I built a standalone harness
  (`/tmp/starv_test.cpp`) against the real transport and ran three
  configurations (60 Hz bursty flood, dense flood ~10 k packets/s, bidirectional
  dense flood). In all three, 100/100 outbound packets enqueued mid-flood were
  delivered; rings dropped only under intentional saturation (the designed
  backpressure). So this is a fragility note, not a demonstrated bug — ENet's
  datagram timing creates ≥ 8 ms gaps even under flood.
- Why it still matters: the M1 host relays 8 players at 60 Hz — the one
  workload with no guaranteed gap. A silently spinning service loop would fill
  the outbox ring (≈0.4 s of traffic) and trigger M2's drops.
- Fix (cheap): bound the inner loop (process at most K events, then drain) or
  drain when `outbox_.Count() > 0` inside the loop.

### MINOR

**m1. No `enet_initialize()` in the game path.** The selftest calls it in
`main` (`selftest_main.cpp`), but no game TU does. On macOS/Linux it is a no-op
(`third_party/enet/unix.c:67`) so M0 acceptance is unaffected; on Windows it is
**required** (`WSAStartup` + `timeBeginPeriod`, `third_party/enet/win32.c:16`)
— the M1 Windows build will fail to create hosts. Fix: call it once at net-module
init (and `enet_deinitialize` at teardown).

**m2. A duplicate `JoinRequest` from one peer assigns a second PlayerId.**
`OnJoinRequest` (`session.cpp:242-298`) never checks
`peerToPlayer_[peerIndex]`; a second request (buggy or malicious client)
lands a second player slot and the first assignment is orphaned — its
`PlayerLeave` on disconnect (`HandleDisconnect`, `session.cpp:156-163`) will
only remove the last mapping. Trust-model defense-in-depth: ignore re-joins or
re-send the existing `JoinAccept`.

**m3. No client deadline on `Connecting`.** The join timeout
(`session.cpp:110-116`) only covers `Connected → Joined`; an unreachable/filtered
host relies on ENet's own ~5 s connect timeout surfacing as `Disconnect` →
`Ended` with no rejection reason (`rejectReasonName_` stays ""). Minor UX; add a
deadline on `Connecting`.

**m4. Drop-newest on full rings is the wrong policy for snapshot streams.**
When the socket thread lags, the *newest* `PlayerState` (the most relevant) is
rejected while stale frames were already sent. ENet's per-channel sequencing
trims stale packets on the wire, which masks this, and my flood tests showed the
ring drains fine in practice — but under a stall, "latest wins" should mean the
sender coalesces per (peer, channel): keep one pending snapshot slot per player
and replace it rather than enqueue a second. Fold into M2's fix.

**m5. `PlayerStateId::Playing` and the `state` roster field are never set.**
`FillWireRoster` hardcodes `Connected` (`session.cpp:466-469`). Harmless in M0;
either use the field in M1 or delete it.

**m6. `net.*` CVars are registered but read by nothing** (`config.cpp`,
`settings.cpp:381-383`); the selftest bypasses them entirely, so the
`ConfigVar<u16>`/`ConfigVar<bool>` path is compile-checked only. Fine for M0 —
note it, and wire `enabled` in M4.

**m7. Plan §4 references `src/dusk/coop/…` (Runtime, context, enemy, combat,
save) — none of that exists on this branch** (the older split-screen coop is on
another branch). M1's "reuse from the existing fork" claims cannot be validated
from this tree; the net module is correctly written against only what exists.

---

## VERIFIED-OK (checked and confirmed)

1. **Build**: `dusk_net_selftest` and the full `dusklight` game target build
   clean on `macos-default-relwithdebinfo`; net TUs produce zero warnings; game
   binary contains `dusk::net::*` symbols and links `libenet.a`.
2. **Selftest exit 0**: 15/15 runs; `leaks --atExit` reports 0 leaks; teardown
   joins every socket thread (log shows "transport socket thread exited" per
   stop, never hangs).
3. **Real-socket handshake**: separate ENet hosts per side; full
   JoinRequest→JoinAccept+WorldInit, both rejects, PlayerLeave relay, SessionEnd
   — all observed in the transport/session logs.
4. **ENet vendoring authentic**: all 19 files byte-identical to the official
   `lsalzman/enet` v1.3.18 tarball (diffed locally); zlib license present
   (`third_party/enet/LICENSE`).
5. **SPSC ring correctness**: standalone stress test (`/tmp/ringtest.cpp`) —
   2,000,000 items through the ring from a hammering producer, strictly
   monotonic FIFO on the consumer, zero corruption, drop-on-full leaves the ring
   intact. Memory-ordering pattern (release-store head after slot write,
   acquire-load head before slot read, `alignas(64)` separation of
   slots/head/tail, `transport.h:83-85`) is the standard correct SPSC pattern.
6. **Protocol conformance §1–§5**: all 15 `MsgType` values contiguous 1–15;
   envelope `u16 type + u16 size`; channels 0 reliable / 1 unreliable-sequenced
   (`ChannelFor`, `enet_host_create(..., 2, ...)` on both sides); wire sizes
   consistent between serializers and `WireSize` (round-trip + explicit size
   checks in the selftest); malformed/truncated/unknown-type packets rejected.
7. **Shutdown**: `Stop()` = stop flag → join → `enet_host_destroy` on the
   socket thread (`transport.cpp:202-208`); idempotent; `Session::Stop` sends
   goodbye messages before teardown; no deadlock in 15 runs including
   destructor-path stops.
8. **NetClock**: exact-interval ticks, no sub-interval ticks, single catch-up
   tick after a ~1 s stall with no burst (`RunClockChecks`), correct for
   variable frame rates.
9. **GAME_SERVICE_MAJOR bump coherent**: upstream `1u` → fork `2u`; SDK trait
   and game module share the macro; no stale hardcoded epoch anywhere.
10. **CMake wiring**: ENet static lib with platform guard (unix.c vs win32.c,
    Ws2_32 on WIN32, `CMakeLists.txt:333-353`); selftest excluded on
    Android/iOS/TVOS (`:743`); `files.cmake` adds the net sources; config
    registration hooked into `registerSettings`.

---

## TOP MUST-FIX BEFORE M1

1. **WorldInit contract** (M1): kill the full-state-sections promise; WorldInit
   stays fixed-size; snapshot-on-join rides the existing per-frame stream.
2. **Reliable-event drops** (M2): separate reliable from snapshot traffic;
   reliable overflow must be an explicit failure, never a silent drop.
3. **Zero-init payload unions** (M3): one-line fix at each `PayloadUnion`
   construction site; restores deterministic wire bytes.
4. **Peer-slot generation guard** (M4): prevents stale-packet misdelivery once
   reconnect/rejoin lands.
5. **Semantic validation** (M5): clamp `jointCount`, range-check
   `assignedPlayerId`.
6. **`enet_initialize` in the game path** (m1) — one line, unblocks Windows.

Items 1–3 are cheap and remove the three ways M1 gets silently wrong data;
items 4–5 are cheap and prevent two classes of subtle desync. None block M0.

---

## SPEC DEVIATIONS vs `00-network.md` §1–§5 (recommendation)

| Spec point | M0 as implemented | Assessment / recommendation |
|---|---|---|
| Channels 0 reliable / 1 unreliable-seq (§1) | ✓ `ChannelFor` + `channelLimit=2` | Conforms |
| All 15 message types (§5) | ✓ 1–15 contiguous | Conforms |
| Framing u16 type + u16 size (§5) | ✓ exact-size validation | Conforms; keep strictness |
| Threading: socket thread ↔ SPSC ↔ game thread (§3) | ✓ | Conforms; drain placement fragile (M6) |
| `WorldInit` = stage, room, spawn, roster, **full state snapshot** (§4/§5) | ✗ Counts only, no sections; cannot grow (M1) | **Deviates by necessity** — amend spec: WorldInit = stage + roster; snapshot-on-join via PlayerState/EnemySnapshot burst |
| `PlayerState` §5 block | + `jointCount` field added | Refinement, matches plan risk R11 — amend §5 to include it |
| `JoinAccept`/`PlayerLeave`/`SessionEnd` payloads | + `reserved`/`reason` fields | Additive, fine |
| `HostAnnounce` UDP broadcast, 44771 (§4) | Not implemented | Out of M0 scope (plan: M4); annotate §4 |
| Config var names (§10) | `net.hostPort`/`net.joinHost`/`net.sessionName`/`net.enabled` vs `host_port`/`join_host`/`session_name`/`enabled` | Cosmetic namespace prefix; note in §10 |

*Review file: `docs/design/mod-coop/review-m0-deepseek-v4-flash.md`. Test
harnesses built for this review live in `/tmp/` (ringtest.cpp, det_test.cpp,
starv_test.cpp); no repository files were modified.*
