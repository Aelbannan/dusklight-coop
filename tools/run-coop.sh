#!/usr/bin/env bash
# run-coop.sh — launch the networked co-op host + client(s) on one machine.
#
# Usage:
#   ./tools/run-coop.sh                 # host + 1 client (2 players), logs to files
#   ./tools/run-coop.sh 4               # host + 3 clients (4 players)
#   ./tools/run-coop.sh --players 4     # same
#   ./tools/run-coop.sh --follow        # ... and tail -f all logs (blocks)
#   ./tools/run-coop.sh --windows       # open each instance in a Terminal.app window
#   ./tools/run-coop.sh --join 192.168.x.x   # client joins a remote host (two machines)
#   ./tools/run-coop.sh --load-save 1   # pass --load-save to skip the title demo
#   ./tools/run-coop.sh --stop          # kill running instances
#
# Env/config defaults (override with flags or edit below):
#   HOST_PORT=44770  CLIENT_PORT=44770  JOIN_HOST=127.0.0.1   (client net.hostPort = the HOST's port)
#   PLAYERS=2   (1 host + PLAYERS-1 clients; max 8 = 1 host + 7 clients)

set -euo pipefail

# Repo root = parent of the script's directory (this script lives in tools/).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BINARY="${REPO_ROOT}/build/macos-default-relwithdebinfo/Dusklight.app/Contents/MacOS/Dusklight"
HOST_PORT="${HOST_PORT:-44770}"
CLIENT_PORT="${CLIENT_PORT:-44770}"
JOIN_HOST="${JOIN_HOST:-127.0.0.1}"
HOST_LOG="${REPO_ROOT}/net-host.log"
PLAYERS="${PLAYERS:-2}"
MAX_PLAYERS=8

FOLLOW=0
WINDOWS=0
LOAD_SAVE=""

usage() {
    sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

stop_all() {
    pkill -f "Dusklight.app/Contents/MacOS/Dusklight" 2>/dev/null \
        && echo "stopped Dusklight instances" \
        || echo "no Dusklight instances running"
    exit 0
}

set_players() {
    local n="${1:?}"
    if ! [[ "${n}" =~ ^[1-9][0-9]*$ ]]; then
        echo "error: players must be a positive integer, got '${n}'" >&2
        exit 1
    fi
    if [ "${n}" -gt "${MAX_PLAYERS}" ]; then
        echo "error: players must be <= ${MAX_PLAYERS} (1 host + $((MAX_PLAYERS - 1)) clients)" >&2
        exit 1
    fi
    PLAYERS="${n}"
}

[ $# -gt 0 ] || true
while [ $# -gt 0 ]; do
    case "$1" in
        --follow) FOLLOW=1 ;;
        --windows) WINDOWS=1 ;;
        --stop) stop_all ;;
        --join) shift; JOIN_HOST="${1:?--join needs an IP}"; CLIENT_PORT="$HOST_PORT" ;;
        --load-save) shift; LOAD_SAVE="--load-save ${1:?--load-save needs a slot number}" ;;
        --players) shift; set_players "${1:?--players needs a count}" ;;
        --help|-h) usage ;;
        [1-9]|[1-9][0-9]) set_players "$1" ;;
        *) echo "unknown option: $1" >&2; usage >&2 ;;
    esac
    shift
done

CLIENT_COUNT=$((PLAYERS - 1))

if [ ! -x "${BINARY}" ]; then
    echo "error: binary not found at ${BINARY}" >&2
    echo "build it first:  ninja -C build/macos-default-relwithdebinfo dusklight" >&2
    exit 1
fi

# The game resolves the disc image (repo-root RVZ) from its working directory.
cd "${REPO_ROOT}"

HOST_ARGS=(--cvar net.enabled=true --cvar net.role=host --cvar net.hostPort="${HOST_PORT}")
CLIENT_ARGS=(--cvar net.enabled=true --cvar net.role=client \
             --cvar net.hostPort="${CLIENT_PORT}" --cvar net.joinHost="${JOIN_HOST}")
[ -n "${LOAD_SAVE}" ] && HOST_ARGS+=("${LOAD_SAVE}") && CLIENT_ARGS+=("${LOAD_SAVE}")

client_log() {
    echo "${REPO_ROOT}/net-client-${1}.log"
}

echo "== dusklight net co-op =="
echo "   players: ${PLAYERS} (1 host + ${CLIENT_COUNT} client(s))"
echo "   host:    ${BINARY} ${HOST_ARGS[*]}"
if [ "${CLIENT_COUNT}" -gt 0 ]; then
    echo "   client:  ${BINARY} ${CLIENT_ARGS[*]}"
    echo "   clients join ${JOIN_HOST}:${HOST_PORT}"
fi

if [ "${WINDOWS}" -eq 1 ]; then
    # macOS: one Terminal.app window per instance, live logs.
    osascript -e "tell application \"Terminal\" to do script \"cd '${REPO_ROOT}' && '${BINARY}' ${HOST_ARGS[*]}\"" >/dev/null
    for ((i = 1; i <= CLIENT_COUNT; i++)); do
        osascript -e "tell application \"Terminal\" to do script \"cd '${REPO_ROOT}' && '${BINARY}' ${CLIENT_ARGS[*]}\"" >/dev/null
    done
    echo "opened $((CLIENT_COUNT + 1)) Terminal window(s); watch for:"
    echo "   'coop: host session started' / 'coop: client session started'"
    echo "   'peer N connected'  →  'puppet for player X active'  →  'apply player X to pos=…'"
    exit 0
fi

# Background + log files.
: > "${HOST_LOG}"
"${BINARY}" "${HOST_ARGS[@]}" >"${HOST_LOG}" 2>&1 &
HOST_PID=$!
echo "host pid ${HOST_PID}   → log ${HOST_LOG}"

CLIENT_PIDS=()
LOG_FILES=("${HOST_LOG}")
for ((i = 1; i <= CLIENT_COUNT; i++)); do
    clog="$(client_log "${i}")"
    : > "${clog}"
    "${BINARY}" "${CLIENT_ARGS[@]}" >"${clog}" 2>&1 &
    CLIENT_PIDS+=("$!")
    LOG_FILES+=("${clog}")
    echo "client ${i} pid ${CLIENT_PIDS[$((i - 1))]} → log ${clog}"
done

echo "watch for: 'coop: … session started', 'peer N connected', 'puppet for player X active', 'apply player X to pos=…'"

if [ "${FOLLOW}" -eq 1 ]; then
    trap 'kill 0' EXIT
    tail -f "${LOG_FILES[@]}"
fi
