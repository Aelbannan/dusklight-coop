#!/usr/bin/env bash
# run-coop.sh — launch the networked co-op host + client on one machine.
#
# Usage:
#   ./tools/run-coop.sh                 # host + client, background, logs to files
#   ./tools/run-coop.sh --follow        # ... and tail -f both logs (blocks)
#   ./tools/run-coop.sh --windows       # open both in two Terminal.app windows
#   ./tools/run-coop.sh --join 192.168.x.x   # client joins a remote host (two machines)
#   ./tools/run-coop.sh --load-save 1   # pass --load-save to skip the title demo
#   ./tools/run-coop.sh --stop          # kill running instances
#
# Env/config defaults (override with flags or edit below):
#   HOST_PORT=44770  CLIENT_PORT=44770  JOIN_HOST=127.0.0.1   (client net.hostPort = the HOST's port)

set -euo pipefail

# Repo root = parent of the script's directory (this script lives in tools/).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BINARY="${REPO_ROOT}/build/macos-default-relwithdebinfo/Dusklight.app/Contents/MacOS/Dusklight"
HOST_PORT="${HOST_PORT:-44770}"
CLIENT_PORT="${CLIENT_PORT:-44770}"
JOIN_HOST="${JOIN_HOST:-127.0.0.1}"
HOST_LOG="${REPO_ROOT}/net-host.log"
CLIENT_LOG="${REPO_ROOT}/net-client.log"

FOLLOW=0
WINDOWS=0
LOAD_SAVE=""

usage() {
    sed -n '2,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

stop_all() {
    pkill -f "Dusklight.app/Contents/MacOS/Dusklight" 2>/dev/null \
        && echo "stopped Dusklight instances" \
        || echo "no Dusklight instances running"
    exit 0
}

[ $# -gt 0 ] || true
while [ $# -gt 0 ]; do
    case "$1" in
        --follow) FOLLOW=1 ;;
        --windows) WINDOWS=1 ;;
        --stop) stop_all ;;
        --join) shift; JOIN_HOST="${1:?--join needs an IP}"; CLIENT_PORT="$HOST_PORT" ;;
        --load-save) shift; LOAD_SAVE="--load-save ${1:?--load-save needs a slot number}" ;;
        --help|-h) usage ;;
        *) echo "unknown option: $1" >&2; usage >&2 ;;
    esac
    shift
done

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

echo "== dusklight net co-op =="
echo "   host:   ${BINARY} ${HOST_ARGS[*]}"
echo "   client: ${BINARY} ${CLIENT_ARGS[*]}"
echo "   client joins ${JOIN_HOST}:${HOST_PORT}"

if [ "${WINDOWS}" -eq 1 ]; then
    # macOS: one Terminal.app window per instance, live logs.
    osascript -e "tell application \"Terminal\" to do script \"cd '${REPO_ROOT}' && '${BINARY}' ${HOST_ARGS[*]}\"" >/dev/null
    osascript -e "tell application \"Terminal\" to do script \"cd '${REPO_ROOT}' && '${BINARY}' ${CLIENT_ARGS[*]}\"" >/dev/null
    echo "opened two Terminal windows; watch for:"
    echo "   'coop: host session started' / 'coop: client session started'"
    echo "   'peer N connected'  →  'puppet for player X active'  →  'apply player X to pos=…'"
    exit 0
fi

# Background + log files.
: > "${HOST_LOG}"
: > "${CLIENT_LOG}"
"${BINARY}" "${HOST_ARGS[@]}" >"${HOST_LOG}" 2>&1 &
HOST_PID=$!
"${BINARY}" "${CLIENT_ARGS[@]}" >"${CLIENT_LOG}" 2>&1 &
CLIENT_PID=$!
echo "host pid ${HOST_PID}   → log ${HOST_LOG}"
echo "client pid ${CLIENT_PID} → log ${CLIENT_LOG}"
echo "watch for: 'coop: … session started', 'peer N connected', 'puppet for player X active', 'apply player X to pos=…'"

if [ "${FOLLOW}" -eq 1 ]; then
    trap 'kill 0' EXIT
    tail -f "${HOST_LOG}" "${CLIENT_LOG}"
fi
