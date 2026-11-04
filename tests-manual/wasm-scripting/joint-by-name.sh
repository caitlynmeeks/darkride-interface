#!/bin/zsh
# An agent setting and reading its own joints by name (joint-by-name.js: Avatar.setJointRotation("RightArm", ...) and
# the rest), then reading them by name in a tight loop while a clip plays, in the wasm sandbox, on either engine, up to
# 90 s. A step that hangs the agent's script shows as its last "step" line with no "done" after it (in the stress
# phase, as its count stopping), and the assignment client's monitor then restarts the agent about 25 s later: more
# than one start is a FAIL too. The assignment clients still running at the end are stopped by pid.
#   tests-manual/wasm-scripting/joint-by-name.sh
# The script is served from this checkout on 127.0.0.1:43110 (http-echo.py). Prints the agent's JOINT-BY-NAME lines;
# exits 0 when its RESULT says PASS. A sandbox that was running before is started again, plain, at the end.
#
# Copyright 2026 Darkride.
# Distributed under the MIT License.
set -e
D=$(cd "$(dirname "$0")" && pwd)
BUILD=$(cd "$D/../../build" && pwd)
ROOT=$HOME/tivoli-sandboxes/wasm
SERVE=http://127.0.0.1:43110/tests-manual/wasm-scripting
OUT=$D/results/joint-by-name

settings() {
    python3 - "$ROOT/domain-config.json" "$1" "$SERVE/joint-by-name.js" <<'PY'
import json, sys
path, mode, agent = sys.argv[1:4]
with open(path) as f:
    config = json.load(f)
if mode == "on":
    config.setdefault("scripts", {})["persistent_scripts"] = [{"url": agent, "num_instances": 1, "pool": ""}]
else:
    config.get("scripts", {}).pop("persistent_scripts", None)
    if config.get("scripts") == {}:
        config.pop("scripts")
with open(path, "w") as f:
    json.dump(config, f, indent=2)
PY
}

stop_sandbox() {
    # the monitor's own assignment clients, by pid: one whose agent hangs may not stop when the monitor does
    local children=""
    [ -f "$ROOT/assignment-client.pid" ] && children=$(pgrep -P "$(cat "$ROOT/assignment-client.pid")" | tr '\n' ' ')
    for part in domain-server assignment-client; do
        [ -f "$ROOT/$part.pid" ] && kill "$(cat "$ROOT/$part.pid")" 2>/dev/null || true
        rm -f "${ROOT:?}/${part:?}.pid"
    done
    for i in $(seq 1 15); do
        pgrep -f "$BUILD/(domain-server|assignment-client)" > /dev/null || return 0
        sleep 1
    done
    for pid in ${=children}; do
        kill -9 "$pid" 2>/dev/null && echo "stopped hung assignment client $pid" >&2
    done
    sleep 1
}

mkdir -p "$OUT"
python3 -I "$D/http-echo.py" 43110 "$D/../.." > /dev/null 2>&1 &
HTTP_PID=$!
trap 'kill $HTTP_PID 2>/dev/null' EXIT
WAS_RUNNING=0
[ -f "$ROOT/domain-server.pid" ] && kill -0 "$(cat "$ROOT/domain-server.pid")" 2>/dev/null && WAS_RUNNING=1
stop_sandbox
settings on
SANDBOX_ASSIGNMENTS=7 "$HOME/git/tivolicloud/domain-sandbox.sh" wasm "$BUILD" 43102 > /dev/null
LOG="$ROOT/logs/assignment-client.log"
for i in $(seq 1 90); do
    sleep 1
    # printed lines only: the log also quotes the script's source as it is downloaded
    grep "JOINT-BY-NAME RESULT" "$LOG" 2>/dev/null | grep -qv '\\n' && break
done
grep -h "JOINT-BY-NAME" "$LOG" | grep -v '\\n' | sed 's/.*JOINT-BY-NAME /JOINT-BY-NAME /' > "$OUT/lines.txt" || true
cp "$LOG" "$OUT/assignment-client.log" 2>/dev/null || true
stop_sandbox
settings off
if [ $WAS_RUNNING = 1 ]; then
    "$HOME/git/tivolicloud/domain-sandbox.sh" wasm "$BUILD" 43102 > /dev/null
fi
echo "== the lines:"
cat "$OUT/lines.txt"
STARTS=$(grep -c "JOINT-BY-NAME joint " "$OUT/lines.txt" || true)
[ "$STARTS" = 1 ] || { echo "FAIL: the agent started $STARTS times (a hung script is restarted)"; exit 1; }
grep -qE "RESULT (.* )?PASS" "$OUT/lines.txt"
