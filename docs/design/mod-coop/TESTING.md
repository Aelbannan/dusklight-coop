# Testing the networked co-op (net-coop)

How to build, run, and verify the networked co-op milestones. Current status
flag at the top; check it before testing.

> **Status: M0–M4 (session polish + room ownership) are landed and reviewed;
> the selftest is green on a forced rebuild.** All 300+ selftest checks pass
> incl. the 1 Hz cadence regression guard, the room-ownership routing
> (CombatIntent to a non-host room owner, same-room snapshot scoping,
> ownership transfer), the join-warp gate, entity-id stability, and the LAN
> discovery announce/receive. M4 adds: join-warp (unlock gate + safe anchor),
> host-leave toast UX, LAN discovery, and per-room enemy authority with
> combat routing to the room owner.

## 0. Config vars (defaults)

| Var | Default | Meaning |
|-----|---------|---------|
| `net.enabled` | `false` | master switch; `false` = byte-for-byte vanilla single-player |
| `net.role` | `"host"` | `"host"` or `"client"` |
| `net.hostPort` | `44770` | host: the port this instance binds (and advertises in the discovery announce). **client: the HOST's port to connect to** — `net.joinHost:net.hostPort`. The client transport never binds, so the same value on both machines is correct (and required on loopback). 44771 is reserved for the discovery listener |
| `net.joinHost` | `127.0.0.1` | what a client connects to (LAN IP for remote) |
| `net.sessionName` | `"Dusklight co-op"` | display name (host: advertised; client: player name) |

All five are also editable in the in-game Settings → Network tab (and the
client's "Discovered Sessions" list shows LAN hosts found by the announce
listener — join by copying the IP into `net.joinHost`).

## 1. Automated selftest (fast, no game)

```sh
ninja -C build/macos-default-relwithdebinfo dusk_net_selftest
./build/macos-default-relwithdebinfo/dusk_net_selftest
```

Expect `PASS: all checks succeeded`, exit 0. Covers: all message round-trips
(byte-identical serialize→deserialize, incl. the M4 RoomOwnership message),
handshake (JoinRequest → JoinAccept+WorldInit, both rejects, PlayerLeave
relay, SessionEnd), roster-refresh on join (3-player visibility), and the
relay policies (PlayerState/PlayerEvent star-relay; CombatIntent routed to
the ROOM owner; EnemySnapshot room-scoped; CombatResult/EnemyEvent
simulcast; TimeSync/WeatherChange host→all), the time/weather contract
(1 Hz TimeSync cadence, thunder/pond/seed tables), M4 room ownership
(sticky first-in, host-default, transfer on leave/disconnect, intent
routing to a non-host owner, same-room snapshot scoping), the join-warp
unlock gate + worldStage carry, entity-id stability across takeover, and
LAN discovery announce/receive over loopback.

## 2. Full build

```sh
ninja -C build/macos-default-relwithdebinfo dusklight
```

Binary: `build/macos-default-relwithdebinfo/Dusklight.app/Contents/MacOS/Dusklight`
The disc image `Legend of Zelda, The - Twilight Princess (USA).rvz` and a save
(`USA/Card A/01-GZ2E-gczelda2.gci`) live in the repo root. Use `--load-save 1`
to get past the title demo.

## 3. Two-instance loopback (one machine)

Run the same app twice; CLI `--cvar` overrides split the roles. Give each
instance its own `net.hostPort` so the two ENet hosts don't clash.

**Terminal 1 — host:**

```sh
build/macos-default-relwithdebinfo/Dusklight.app/Contents/MacOS/Dusklight \
  --cvar net.enabled=true --cvar net.role=host --cvar net.hostPort=44770
```

**Terminal 2 — client:**

```sh
build/macos-default-relwithdebinfo/Dusklight.app/Contents/MacOS/Dusklight \
  --cvar net.enabled=true --cvar net.role=client --cvar net.hostPort=44770 \
  --cvar net.joinHost=127.0.0.1
```

Note `net.hostPort` on the client is the **host's** port (the connect target:
`net.joinHost:net.hostPort`); the client transport never binds, so host and
client can share 44770 on one machine. Port 44771 is the discovery
listener's — a second client on the same machine is fine (SO_REUSEADDR).

Coop/session logs print to the terminal. Watch for, in order:

1. `coop: host session started (port 44770)` / `coop: client session started`
2. `discovery: announcing 'Dusklight co-op' on port 44770` (host) and
   `discovery: found session ... at 127.0.0.1:44770` (client — same machine
   discovery works via the loopback announce)
3. On join: `puppet for player X active (pid N)` on **both** instances
4. `apply player X to pos=(…)` — the client applies the host's pose, and vice
   versa (the applied position must equal the received position)

**In-game checks per milestone:**

| Milestone | What to verify |
|-----------|----------------|
| M1 players | Second player's Link appears, mirrors the other's movement/pose exactly; frozen during the local player's cutscenes; join/leave clean (puppet spawns/despawns); save file mtime unchanged (no host-save corruption) |
| M2 enemies | Host fights an enemy → client sees the same enemy at the same HP; a client's hits kill it (host applies); drops spawn on BOTH; room-clear doors open together |
| M2.5 targeting | Kite a whitelisted enemy (Armos `E_AI`, Kargorok `E_YC`, …) past the client's Link → it turns and attacks the NEAREST player, not just the host |
| M3 time/weather | Same sky on both; rain arrives on both; a cutscene freezes the clock on both; a stage transition re-asserts the same time |
| M4 join-warp | Client joins a host in a stage the client's save hasn't reached → the client STAYS put (no crash, no warp), a "Host in a far-away stage" toast appears, and the players become visible only when in a shared stage. Joining from a save that HAS reached the stage → the client warps to the host's stage beside the host |
| M4 host-leave | Host quits or kills the process → client shows a "Host left/disconnected" toast, puppets despawn, and single-player continues normally |
| M4 ownership | Two players in different rooms of the same stage: each room's enemies sim on that room's owner (first player in, host wins its own room); the other player sees them frozen; combat from either side lands via the room-owner route; the owner leaving transfers the room |
| M4 LAN discovery | Host running: client's log shows `discovered session 'Dusklight co-op' at <ip>:<port>`; the Settings → Network tab lists it |
| Save integrity | `USA/Card A/*.gci` mtime unchanged across all runs |

## 4. Two machines on LAN

Same as §3, but the client points at the host's LAN IP:

```sh
… --cvar net.role=client --cvar net.hostPort=44770 --cvar net.joinHost=192.168.x.x
```

Same ports are fine (different machines). Both need their own copy of the game
+ disc + save.

## 5. Regression sanity

With `net.enabled=false` (default) the game must play exactly like vanilla —
that is the baseline every milestone must preserve. Boot it once with no cvars
and spot-check a normal stage load.

## 6. If something fails

- Selftest red after a milestone lands: check whether the milestone is
  mid-implementation (uncommitted changes) before assuming a real regression.
- Build fails to link: force-rebuild the touched TUs (`touch` the files) —
  incremental builds have masked failures in this project before.
- Puppet invisible / wrong room: the sender gate is same-room scoped; both
  players must be in the same room.
- Logs: coop lines go to the terminal via the dusk logging system
  (`CoopLog.info`); add `--console` on Windows for a visible console.
