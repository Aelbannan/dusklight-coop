# Adversarial review — M0 network layer

Reviewer: **z-ai/glm-5.2** (adversarial pass)
Scope: M0 of the networked co-op, branch `net-coop`, commits
`b1ca17fabd..ee4e5e088e` (six commits).
Spec contract: `docs/design/mod-coop/00-network.md` §1–§5, `implementation-plan.md`
Rev 3 §5 (M0).
Method: read design + code, then **built and ran** the selftest on the existing
macOS CMake+Ninja setup (`build/macos-default-relwithdebinfo`).

---

## VERDICT

**M0 meets its acceptance criteria as implemented.** Concretely, against
`implementation-plan.md` §5 M0 acceptance:

| Acceptance criterion | Status | Evidence |
|---|---|---|
| mod-free build runs (net module compiles into the game binary) | ✅ | `build/.../CMakeFiles/dusklight.dir/src/dusk/net/{config,session,transport,protocol,clock}.cpp.o` all built; `dusklight` links with `enet` (`CMakeLists.txt:362`). |
| netcode skeleton compiles | ✅ | Clean rebuild of `dusk_net_selftest` produced **zero warnings** (`ninja dusk_net_selftest`). |
| LAN host/client handshake works with no game code attached | ✅ | `./dusk_net_selftest` → **`PASS: all checks succeeded`, exit 0**, 5/5 repeat runs, `leaks` reports **0 leaked bytes**. |

The handshake is **genuinely exercised over real sockets**: the demo spawns a
host `Session` (real ENet host bound to an ephemeral UDP port on 127.0.0.1,
e.g. `53916`) and separate client `Session`s, each with its own ENet host, that
`enet_host_connect` to it. The transport log shows `peer N connected` /
`disconnected`, and the session log shows the full
`JoinRequest → JoinAccept + WorldInit`, `JoinReject` (version + full),
`PlayerLeave` relay, and host `SessionEnd` round-trips. This is inter-host UDP
over loopback, not a same-host loopback object — it tests the real ENet path.

That said, M0 is a foundation with **two forward-compatibility problems that
will force a redesign before M1 ships**, plus several minor issues. None block
accepting M0; they block *building M1 on top of M0 unchanged*. Details below.

---

## RANKED FINDINGS

### MAJOR-1 — Single SPSC ring mixes reliable and unreliable traffic; overflow drops reliable events silently

**Claim:** `Transport` has one inbox ring and one outbox ring
(`include/dusk/net/transport.h:177-178`). When either is full, `Push` returns
false and the message is **dropped** with only a relaxed counter bump and a
log line (`src/dusk/net/transport.cpp:126-128`, `:159`, `:172`, `:187-188`).
The drop is **drop-newest** (the producer's `Push` is rejected; the ring is
left untouched — `transport.h:57`). There is no distinction between reliable
control/event/combat messages (channel 0) and unreliable snapshots (channel 1).

**Why it matters:** ENet only provides reliability *after* `enet_peer_send`. A
reliable message that never reaches the socket thread (because the outbox ring
was momentarily full) is **lost forever** — no retry, no ack, the application
never knows. M1 runs players+enemies at 60 Hz (`00-network.md` §6): ~7 players
× 60 + ~40 enemies × 60 ≈ 47 outbound messages/frame. The outbox capacity is
128 (`transport.h:177`), i.e. ~2.7 frames of headroom. Any socket-thread stall
longer than that (ENet flush, OS scheduling, a GC/debug pause) fills the ring
and starts dropping. A dropped `CombatIntent`/`EnemyEvent`/`PlayerEvent` on the
reliable channel is a correctness bug, not a cosmetic one. Symmetric problem on
the inbox: a dropped `JoinAccept`/`CombatResult` is lost.

**Fix (before M1):** split the rings by channel/priority — a small ring for
channel-0 reliable (never silently dropped; on full, either block the game
thread for one frame, replace-oldest, or assert) and a large ring for
channel-1 unreliable snapshots (replace-oldest is the right policy there — see
MAJOR-2). At minimum, make channel-0 overflow an explicit failure the session
layer must handle, not a silent `outboundDropped_++`.

---

### MAJOR-2 — Drop policy is drop-newest, which is the wrong choice for snapshot traffic

**Claim:** `SPSCRing::Push` rejects the new item when full (`transport.h:54-63`).
For the unreliable snapshot channel this means the **freshest** snapshot is
discarded and the consumer keeps draining **stale** entries — the opposite of
what a pose-sync pipeline wants (latest packet IS the state, `00-network.md`
§6).

**Why it matters:** under transient ring pressure the remote puppet receives
stale poses instead of the newest one; visible jitter/regression exactly where
M1's "no interpolation, latest-state" model is most latency-sensitive.

**Fix:** for the snapshot ring use a drop-oldest ring (consumer-side overwrite
of the tail, or a producer that evicts the oldest slot on full). Reliable
control traffic should not share that ring (MAJOR-1).

---

### MAJOR-3 — `kMaxMessageSize = 4096` and the fixed-size ring slots make `WorldInit`'s full-state snapshot infeasible

**Claim:** `protocol.h:57` sets `kMaxMessageSize = 4096`, sized for "the Rev 3
D4 raw matrix pose (~2.7 KB) plus envelope" — i.e. for a **single**
`PlayerState`. `Transport::Send` rejects anything larger (`transport.cpp:117`).
`Transport`'s inbox/outbox slots are `u8 data[kMaxMessageSize]` (4096 B each,
`transport.h:106`, `:114`). Meanwhile `00-network.md` §4/§5 and the plan M1
(`implementation-plan.md` §5 M1 "Wire") specify `WorldInit` carries a **full
state snapshot** (roster + all player + enemy states), and `protocol.h`
literally comments "M1 appends player/enemy state snapshots as count-prefixed
sections after the roster."

**Why it matters:** 7 remote players × ~2.7 KB raw-matrix `PlayerState` ≈
19 KB just for players, plus enemies — far over 4096. So a single
`WorldInit` cannot carry the full snapshot through this transport. Either M1
raises `kMaxMessageSize` (which also inflates every one of the 128 inbox +
128 outbox slots — 128×2×Nbytes of resident memory, ~1 MB today at 4096, ~8 MB
at 32 KB, 99% of it unused since `PlayerState` is 2.7 KB), or `WorldInit` must
be fragmented into many messages. Neither is addressed by the M0 design.

**Fix (before M1):** decide now. Recommended: keep `kMaxMessageSize` at ~4 KB,
make `WorldInit` a *multi-message* sequence (a `WorldInitHeader` + a stream of
`PlayerState`/`EnemySnapshot` "initial state" messages on the reliable
channel, terminated by a `WorldInitDone`), and do not grow the ring slots. If
you instead keep one big `WorldInit`, the ring slot type must become
variable-size (offset/length into a slab) rather than a fixed array.

---

### MAJOR-4 — `DeserializeMessage` requires `payloadSize == WireSize(type)` exactly, blocking variable-length messages

**Claim:** `src/dusk/net/protocol.cpp:250`:
`if (payloadSize != WireSize(type) || r.remaining() < payloadSize) return false;`.
This is great defense for M0's all-fixed-size protocol, but the plan's
`WorldInit` ("count-prefixed sections") and any future variable-length section
(`PlayerState` jointCount-driven, batched enemy snapshots) will have a payload
size that is not a compile-time constant.

**Why it matters:** M1 cannot "just append sections to WorldInit" — the
deserializer will reject it. The envelope already carries a `u16 size` field
that *could* support variable payloads, but the strict equality check forbids
it. This is a known consequence of MAJOR-3, but it is an independent code site
that must change.

**Fix:** for message types that are variable-length, replace the exact match
with `r.remaining() >= payloadSize && payloadSize <= MaxWireSize(type)` and let
the per-type deserializer consume exactly `payloadSize` bytes (the envelope
already records the real length). Keep the exact match for the fixed types.
Bump `kProtocolVersion` when the wire layout of any existing type changes.

---

### MINOR-1 — `Transport` uses `kInvalidPlayerId` for peer-slot sentinels; the header defines `kInvalidPeer`

**Claim:** `include/dusk/net/transport.h:36` defines `constexpr u8 kInvalidPeer
= 0xFF` and uses it for `InboundPacket.peerIndex`/`OutboundPacket.peerIndex`
defaults (`:103`, `:111`). But `src/dusk/net/transport.cpp` uses
`kInvalidPlayerId` (from `protocol.h`, also 0xFF) for peer-slot returns:
`AssignPeerSlot` returns `kInvalidPlayerId` (`:139`), the CONNECT handler
compares `index == kInvalidPlayerId` (`:151`).

**Why it matters:** pure naming/layering confusion — the transport peer-slot
space is not the player-id space, even though both happen to use 0xFF today.
A future change to either sentinel silently breaks the other. Also `AssignPeerSlot`
returning a *player* id sentinel from a *peer*-slot function is a smell.

**Fix:** in `transport.cpp` use `kInvalidPeer` for all peer-slot sentinels;
delete the unused-in-cpp duplication. (Functionally safe today; cosmetic.)

---

### MINOR-2 — `running_` is only set true inside the socket thread; `IsRunning()` is false between `StartHost` and thread start

**Claim:** `StartHost`/`StartClient` (`transport.cpp:30`, `:65`) do not set
`running_`; it is set true at the top of `SocketThreadMain` (`:102`) and false
at the bottom (`:188`). `Stop` sets it false after join (`:95`).

**Why it matters:** a caller that does `if (t.IsRunning()) ...` immediately
after `StartHost()` races the thread spawn and may see false. Not currently
used by `Session` (it keys off `SessionState`), but it is a public API.

**Fix:** set `running_ = true` under `lifecycleMutex_` before `std::thread`
construction in both `Start*` paths (and leave the in-thread set as a no-op or
assert). Trivial.

---

### MINOR-3 — Spec drift: `PlayerState` and `EnemySnapshot` field order differs from `00-network.md` §5; `jointCount` is not in the §5 block

**Claim:** `00-network.md` §5's `PlayerState` block lists fields in the order
`playerId, scene, pos, rot, joints, upperLimbRot, movementFlags, form,
cosmetics, stateFlags, itemAction, invincibility`. The M0 serializer
(`protocol.cpp:96-118`) writes scalars first (`playerId, scene, form,
movementFlags, jointCount, cosmetics, itemAction, invincibility, reserved,
stateFlags`) then `pos, rot, upperLimbRot, joints` — a different wire order.
Likewise `EnemySnapshotMsg` reorders `pos`/`angle`/`hp` vs the §5 block. Also
`PlayerStateMsg.jointCount` (`protocol.h:175`) is not listed in the §5 struct
block (the plan R11 adds it, but the spec doc was not amended).

**Why it matters:** both ends agree (the selftest's serialize→deserialize→
re-serialize byte-identity check passes for all 15 types), and
`kProtocolVersion` guards a peer built against a different layout, so this is
not a correctness bug. It *is* documentation drift: anyone implementing a peer
or a network sniffer from `00-network.md` §5 will get the byte layout wrong.

**Fix:** amend `00-network.md` §5 to show the actual M0 wire order (or state
that the struct block is illustrative and the version-gated serializer is
normative), and add `jointCount u8` to the `PlayerState` block.

---

### MINOR-4 — `HostAnnounce` (UDP broadcast, port 44771) from §4 is not implemented

**Claim:** `00-network.md` §4 lists `HostAnnounce` (UDP broadcast, port 44771)
as session step 1; `config.h` even comments "44771 is reserved for the v1
broadcast announce." M0 only implements manual IP join (`joinHost` CVar,
`Session::StartClient`). No broadcast/announce code exists.

**Why it matters:** none for M0 — the plan defers LAN discovery to M4
(`implementation-plan.md` §5 M4, "LAN discovery + manual IP config UI") and the
M0 acceptance only requires "handshake works." This is *intentional* drift,
not an oversight. Noting it so M4 picks it up and so the §4 table isn't read as
"implemented in M0."

**Fix:** none now; track for M4. Optionally annotate §4's table with the
milestone that implements each row.

---

### MINOR-5 — Oversized inbound ENet packets are dropped without a metric

**Claim:** `transport.cpp:168` guards
`event.packet->dataLength <= kMaxMessageSize`; an oversized packet is
`enet_packet_destroy`'d but neither counted nor logged. Only inbox-*full*
drops increment `inboundDropped_`.

**Why it matters:** a misbehaving/legacy peer sending a >4 KB packet would be
silently ignored, making a future inter-version failure invisible in
`--net-stats`.

**Fix:** add an `inboundOversized_` counter (or fold into `inboundDropped_`)
and a debug log line.

---

### MINOR-6 — `Session::Stop()` is not idempotent-by-guard; relies on branch conditions

**Claim:** `session.cpp:80`'s guard is
`if (role_ == SessionRole::None && state_ == SessionState::Idle) return;`.
After a real `Stop`, `state_` becomes `Ended` but `role_` stays `Host`/`Client`,
so the guard does not fire on a second `Stop` — it falls through to
`transport_.Stop()` (which is idempotent and returns early because `host_ ==
nullptr`) and re-sets `state_ = Ended`.

**Why it matters:** it is *safe* (the inner branches check `state_ ==
Listening`/`Joined` and skip, and `transport_.Stop()` is idempotent), but it's
fragile — it depends on every Stop-only side effect being independently
idempotent. A future Stop side effect that isn't would double-fire.

**Fix:** guard on `state_ == Idle || state_ == Ended` (or a separate
`stopped_` flag).

---

### MINOR-7 — `clock.cpp` is an empty translation unit

**Claim:** `src/dusk/net/clock.cpp` is two comment lines and an empty
namespace; `NetClock` is header-only (`clock.h`).

**Why it matters:** harmless (the comment explains it exists for "stable CMake
wiring"), but it's a build-time no-op shipped as a source file. Either fold
into the selftest/game file lists via the header only, or keep it and drop the
apology comment.

**Fix:** cosmetic — leave as-is or remove from the selftest target's source
list (`CMakeLists.txt:741`) since it contributes no symbols.

---

## VERIFIED-OK (claims checked and confirmed)

- **Selftest builds and exits 0.** `ninja dusk_net_selftest` clean (0 warnings);
  `./dusk_net_selftest` → `PASS: all checks succeeded`, exit 0, repeatable
  (5/5). `leaks --atExit` → **0 leaks for 0 total leaked bytes** (clean thread
  join / ENet destroy).
- **Handshake is real inter-host UDP over loopback**, not an in-object
  loopback: host binds an ephemeral port (observed `127.0.0.1:53916`), clients
  `enet_host_connect` to it; transport log shows `peer N connected/disconnected`
  for distinct peers 0–3.
- **Protocol round-trips all 15 message types** with byte-identical
  serialize→deserialize→re-serialize, plus rejects truncated header, lying
  size, and unknown type (`selftest_main.cpp` `RunProtocolChecks`).
- **Wire sizes verified** by the round-trip size check: `JoinAccept` = 324
  (`1+3+20+8+4+8*36`), `PlayerState` = 279 (`11+4+12+6+6+40*6`), matching
  `WireSize` (`protocol.cpp:208-236`).
- **Channel mapping matches §5** exactly: `PlayerState`/`EnemySnapshot`/
  `TimeSync` → `kChannelUnreliable`; all control/events/combat/weather →
  `kChannelReliable` (`protocol.h:497-505`, selftest asserts this).
- **ENet channel flags are correct**: channel 0 → `ENET_PACKET_FLAG_RELIABLE`;
  channel 1 → flags 0 = unreliable **sequenced** (no `ENET_PACKET_FLAG_UNSEQUENCED`),
  matching "unreliable sequenced" in §1 (`transport.cpp:219-220`).
- **SPSC ring correctness**: power-of-two capacity, single-wasted-slot full
  check (`next == tail`), empty check (`tail == head`), mask-wrap on both
  indices, `alignas(64)` on slots/head/tail to prevent false sharing, and the
  canonical producer/consumer memory ordering (relaxed load + acquire on the
  opposite index; release on the store) (`transport.h:54-80`). The 4 KB
  non-atomic slot copy is safe because producer and consumer never touch the
  same slot simultaneously.
- **Serializers are bounds-checked and UB-free**: `ByteWriter`/`ByteReader`
  check `pos_ + len > capacity_`/`len_` on every primitive
  (`protocol.h:430-483`); `f32`↔`u32` via `std::memcpy` (no strict-aliasing UB);
  `WriteFixedString`/`ReadFixedString` always NUL-terminate
  (`protocol.h:456-471`, `:484-494`).
- **Endianness**: all wire primitives are explicitly little-endian bytewise
  (`protocol.h:437-462`), independent of host byte order. Correct for a
  mixed-arch LAN (e.g. an arm64 Mac and an x86 guest).
- **Thread lifecycle is deadlock-free**: `Transport::Stop` joins the socket
  thread while holding `lifecycleMutex_`, but `SocketThreadMain` never acquires
  that mutex, so there is no lock-order inversion; `enet_host_destroy` runs on
  the socket thread after the service loop exits (header comment + `transport.cpp:183-186`).
- **PlayerId assignment is race-free and correct**: host self = 0
  (`session.cpp:42`), clients assigned the first free slot 1..7 on the game
  thread in `OnJoinRequest` (`session.cpp:212-218`); rejoin reuses a freed
  slot (selftest "D assigned PlayerId 1" after A leaves).
- **Disconnect is robust to lost reliable messages**: `HandleDisconnect`
  (host) calls `RemovePlayer(broadcastLeave=true)` regardless of whether the
  `PlayerLeave` message arrived (`session.cpp:154-167`); client treats host
  disconnect as `Ended` (`session.cpp:171-174`) independent of `SessionEnd`.
  So a dropped `PlayerLeave`/`SessionEnd` still converges via ENet's
  disconnect event.
- **NetClock catch-up is correct under variable frame rates**: 60-Hz sample
  → one tick each; sub-interval samples → no tick; a ~1 s stall → exactly one
  tick (steps advanced, no burst); cadence resumes at the next boundary
  (verified by `RunClockChecks`).
- **GAME_SERVICE_MAJOR bump is correct hygiene**: bumped 1→2 in
  `sdk/include/mods/svc/game.h` with a clear comment; nothing else in the SDK
  references the numeric value, so no co-dependent change is required
  (grep of the repo finds only the `#define` and the comment).
- **ENet vendoring is authentic 1.318**: all 8 `.c` files and 8 `include/enet/*.h`
  headers plus `LICENSE` are **byte-identical** to the
  `lsalzman/enet` `v1.3.18` tag (verified via `diff` against the raw GitHub
  sources). zlib-style LICENSE present. Platform split is correct: `win32.c`
  compiled only on `WIN32`, `unix.c` otherwise (`CMakeLists.txt:344-348`),
  and `unix.c`'s `__APPLE__` block sets the right `HAS_*` feature macros.
- **Config CVars are wired**: `dusk::net::config::registerConfig()` is actually
  called from `dusk::registerSettings()` (`src/dusk/settings.cpp:383`), so
  `net.enabled`/`hostPort`/`joinHost`/`sessionName` are registered (M4 will
  read them).
- **Net module compiles in the game context**, not just the selftest:
  `dusklight.dir/src/dusk/net/config.cpp.o` etc. are part of the game target
  (the selftest deliberately omits `config.cpp` and provides an aurora logging
  shim, keeping it standalone — clean separation).

---

## TOP MUST-FIX BEFORE M1

1. **MAJOR-1/2 (rings):** split the outbox/inbox per channel and switch the
   snapshot ring to drop-oldest. A single drop-newest ring that can silently
   lose a `CombatIntent`/`EnemyEvent` is not acceptable once combat flows.
2. **MAJOR-3/4 (message size + strict deserialization):** decide the
   `WorldInit`-full-snapshot transport strategy *before* writing the M1
   serializer. As written, `WorldInit` cannot fit a full state snapshot in a
   4096-byte slot, and `DeserializeMessage`'s exact `payloadSize == WireSize`
   check forbids the count-prefixed sections the plan already calls for.
3. **MINOR-3 (spec drift):** amend `00-network.md` §5 to the actual M0 wire
   order and add `jointCount`, so M1's serializer work has a normative
   reference.

---

## Spec deviations from `00-network.md` §1–§5

| § | Deviation | Severity | Recommendation |
|---|---|---|---|
| §4 | `HostAnnounce` (UDP broadcast, 44771) not implemented | MINOR-4 | Intentional — deferred to M4 per plan. Annotate §4's table with implementing milestone. |
| §5 | `PlayerState`/`EnemySnapshot` field order differs from the §5 struct blocks | MINOR-3 | Amend §5 to the actual wire order, or mark the serializer as normative. Bump `kProtocolVersion` on any M1 layout change. |
| §5 | `PlayerState.jointCount` added (not in §5 block) | MINOR-3 | Add `jointCount u8` to the §5 block (plan R11 already mandates it). |
| §5 | `WorldInit` "full state snapshot" / count-prefixed sections not yet transportable | MAJOR-3/4 | Resolve the size/path question before M1 (see TOP MUST-FIX #2). |

No deviations found in: channel assignment (§1), topology/star + threaded
handoff shape (§3), PlayerId space 0–7 (§4), the 15 message types (§5), the
`u16 type + u16 size` envelope (§5), or the 60-Hz no-interpolation cadence
decision (§6, verified via `NetClock`).
