# Testing the networked co-op (net-coop)

How to build, run, and verify the networked co-op. Current status flag at
the top; check it before testing.

> **Live model: parallel worlds + player/horse puppets. Protocol v11 (9
> message types).** Every machine sims its own enemies, combat, clock, sky,
> and chests. Only Link/Epona puppets cross the wire (star-relayed
> `PlayerState` / `PlayerEvent` / `HorseState`). Stay-put join: a joining
> client stays in its own stage; players meet by traveling. Ghost
> spectating, LAN discovery, and the M2/M3/M4 owner-authoritative
> enemy/combat/room-ownership stack were designed then shredded or never
> built — do not test them. v10 added a stage name on `PlayerEvent`; v11
> added `HorseState`.
>
> Historical notes (not live product): M4.5 stay-put join; M4.6 session
> restart via `Session::Stop()`; M5.1 shredded the enemy/ownership stack.
> Those reviews still describe dead combat/time/discovery code.

## 0. Config vars (defaults)

| Var | Default | Meaning |
|-----|---------|---------|
| `net.autoConnect` | `false` | On launch, start a **client** to `joinHost:hostPort`. Does not auto-host. In-game toggle does not start/stop a session. |
| `net.connected` | `false` | Not persisted by the game. `--cvar net.connected=true` starts a session this run using `net.role`. |
| `net.role` | `"host"` | `"host"` or `"client"`; set by Host / Connect. Autostart from `autoConnect` always connects as client. |
| `net.hostPort` | `44770` | Host: bind port. Client: host's port (`joinHost`:`hostPort`). Client transport never binds. |
| `net.joinHost` | `127.0.0.1` | Client connect IP or hostname (not `host:port`). |
| `net.sessionName` | `"Dusklight co-op"` | Host roster name / client player name |

Legacy `net.enabled` in `config.json` migrates: `true` + `role=client` → `autoConnect`; `true` + `role=host` does not auto-host. `--cvar net.enabled=true` still starts a session this process.

These are editable in Settings → Network (except `connected` / `role` as standalone rows; Host and Connect set role).

## 1. Automated selftest (fast, no game)

```sh
ninja -C build/macos-default-relwithdebinfo dusk_net_selftest
./build/macos-default-relwithdebinfo/dusk_net_selftest
```

Expect `PASS: all checks succeeded`, exit 0. Covers: all 9 v11 message
round-trips (byte-identical serialize→deserialize, including `HorseState`),
NaN/Inf pose rejection, handshake (JoinRequest → JoinAccept+WorldInit, both
rejects, PlayerLeave relay, SessionEnd), roster-refresh on join (3-player
visibility), PlayerState/PlayerEvent/HorseState star-relay, snapshot rings
that replace the newest matching `playerId` under pressure, the worldStage
carry (stay-put join), and the session-restart check (a client whose session
ended from the host's side — graceful SessionEnd and hard connection loss —
gets its transport torn down by `Stop()` and starts a second/third session
in the same process). Combat, ownership, time/weather, ghost, and LAN
discovery messages are out of range on the wire and must stay rejected.

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
  --cvar net.connected=true --cvar net.role=host --cvar net.hostPort=44770
```

**Terminal 2 — client:**

```sh
build/macos-default-relwithdebinfo/Dusklight.app/Contents/MacOS/Dusklight \
  --cvar net.connected=true --cvar net.role=client --cvar net.hostPort=44770 \
  --cvar net.joinHost=127.0.0.1
```

Note `net.hostPort` on the client is the **host's** port (the connect target:
`net.joinHost`:`net.hostPort`); the client transport never binds, so host and
client can share 44770 on one machine.

Coop/session logs print to the terminal. Watch for, in order:

1. `coop: host session started (port 44770)` / `coop: client session started`
2. On join: `puppet for player X active (pid N)` on **both** instances
3. The remote Link moves with the other player (no 1 Hz apply log; watch the
   puppet on screen)

**In-game checks (live model only):**

| What | What to verify |
|------|----------------|
| Players | Second player's Link appears, mirrors the other's movement/pose; join/leave clean (puppet spawns/despawns); save file mtime unchanged (no host-save corruption) |
| Appearance | Remote tunic/sword/shield match after stay-put join; swapping clothes on one machine does not swap the local player's |
| Horse | Remote Epona follows the rider; local Epona saddle/bag materials stay correct while a horse puppet is on screen |
| Stay-put join | Client joins a host in ANOTHER stage → both players STAY in their own stages (no warp); puppets stay hidden until both travel to a shared stage |
| Host-leave | Host quits or kills the process → client shows a "Host left/disconnected" toast, puppets despawn, and single-player continues normally |
| LAN join | Client `--cvar net.joinHost=127.0.0.1` (or the host's LAN IP) + Connect / `net.connected`. There is no LAN discovery broadcast — the IP is typed. |
| Parallel worlds | Enemies, chests, clock, and sky stay **local**. Do not expect shared HP, shared drops, shared rain, or enemy aggro on the remote puppet. |
| Save integrity | `USA/Card A/*.gci` mtime unchanged across all runs |

## 4. Two machines on LAN

Same as §3, but the client points at the host's LAN IP:

```sh
… --cvar net.role=client --cvar net.hostPort=44770 --cvar net.joinHost=192.168.x.x
```

Same ports are fine (different machines). Both need their own copy of the game
+ disc + save.

## 5. Regression sanity

With no session (default: `net.autoConnect=false`, no Host/Connect) the game
must play exactly like vanilla — that is the baseline every milestone must
preserve. Boot it once with no cvars and spot-check a normal stage load.

## 6. If something fails

- Selftest red after a milestone lands: check whether the milestone is
  mid-implementation (uncommitted changes) before assuming a real regression.
- Build fails to link: force-rebuild the touched TUs (`touch` the files) —
  incremental builds have masked failures in this project before.
- Puppet invisible / wrong room: the sender gate is same-room scoped; both
  players must be in the same room.
- Logs: coop lines go to the terminal via the dusk logging system
  (`CoopLog.info`); add `--console` on Windows for a visible console.
