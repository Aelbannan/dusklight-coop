# Testing the networked co-op (net-coop)

How to build, run, and verify the networked co-op milestones. Current status
flag at the top; check it before testing.

> **Status: M3 and M3.5 (time & weather) are landed and reviewed; the selftest
> is green on a forced rebuild.** M0–M3.5 behavior is verified working (218/218
> selftest checks incl. the 1 Hz cadence regression guard). M4 (session polish +
> room ownership) is next.

## 0. Config vars (defaults)

| Var | Default | Meaning |
|-----|---------|---------|
| `net.enabled` | `false` | master switch; `false` = byte-for-byte vanilla single-player |
| `net.role` | `"host"` | `"host"` or `"client"` |
| `net.hostPort` | `44770` | the port this instance binds |
| `net.joinHost` | `127.0.0.1` | what a client connects to (LAN IP for remote) |
| `net.sessionName` | `"Dusklight co-op"` | display name |

## 1. Automated selftest (fast, no game)

```sh
ninja -C build/macos-default-relwithdebinfo dusk_net_selftest
./build/macos-default-relwithdebinfo/dusk_net_selftest
```

Expect `PASS: all checks succeeded`, exit 0. Covers: all message round-trips
(byte-identical serialize→deserialize), handshake (JoinRequest →
JoinAccept+WorldInit, both rejects, PlayerLeave relay, SessionEnd),
roster-refresh on join (3-player visibility), and the relay policies
(PlayerState/PlayerEvent star-relay; CombatIntent NOT relayed; EnemySnapshot
owner→clients; CombatResult/EnemyEvent simulcast; TimeSync/WeatherChange
host→all, client-sent ones rejected — once M3 lands).

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
  --cvar net.enabled=true --cvar net.role=client --cvar net.hostPort=44771 \
  --cvar net.joinHost=127.0.0.1
```

Coop/session logs print to the terminal. Watch for, in order:

1. `coop: host session started (port 44770)` / `coop: client session started`
2. On join: `puppet for player X active (pid N)` on **both** instances
3. `apply player X to pos=(…)` — the client applies the host's pose, and vice
   versa (the applied position must equal the received position)

**In-game checks per milestone:**

| Milestone | What to verify |
|-----------|----------------|
| M1 players | Second player's Link appears, mirrors the other's movement/pose exactly; frozen during the local player's cutscenes; join/leave clean (puppet spawns/despawns); save file mtime unchanged (no host-save corruption) |
| M2 enemies | Host fights an enemy → client sees the same enemy at the same HP; a client's hits kill it (host applies); drops spawn on BOTH; room-clear doors open together |
| M2.5 targeting | Kite a whitelisted enemy (Armos `E_AI`, Kargorok `E_YC`, …) past the client's Link → it turns and attacks the NEAREST player, not just the host |
| M3 time/weather | Same sky on both; rain arrives on both; a cutscene freezes the clock on both; a stage transition re-asserts the same time |
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
