#!/bin/zsh
# An agent looping the stock walk and run from frame 0, then over the client's frame ranges (walk-frames-body.js), seen
# by a second agent through AvatarList (walk-frames-watch.js: the joints' jumps from one update to the next), in the
# wasm sandbox, on either engine, up to 90 s. Frame 0 of the stock clips is a T-pose: played from 0, a loop flashes it.
#   tests-manual/wasm-scripting/walk-frames.sh
# Both scripts are served from this checkout on 127.0.0.1:43110 (http-echo.py). Prints the agents' WALK-FRAMES lines;
# exits 0 when the watcher's RESULT says PASS. A sandbox that was running before is started again, plain, at the end.
#
# Copyright 2026 Darkride.
# Distributed under the MIT License.
set -e
D=$(cd "$(dirname "$0")" && pwd)
BUILD=$(cd "$D/../../build" && pwd)
ROOT=$HOME/tivoli-sandboxes/wasm
SERVE=http://127.0.0.1:43110/tests-manual/wasm-scripting
OUT=$D/results/walk-frames

settings() {
    python3 - "$ROOT/domain-config.json" "$1" "$SERVE/walk-frames-body.js" "$SERVE/walk-frames-watch.js" <<'PY'
import json, sys
path, mode, body, watch = sys.argv[1:5]
with open(path) as f:
    config = json.load(f)
if mode == "on":
    config.setdefault("scripts", {})["persistent_scripts"] = [{"url": url, "num_instances": 1, "pool": ""}
                                                              for url in (body, watch)]
else:
    config.get("scripts", {}).pop("persistent_scripts", None)
    if config.get("scripts") == {}:
        config.pop("scripts")
with open(path, "w") as f:
    json.dump(config, f, indent=2)
PY
}

stop_sandbox() {
    for part in domain-server assignment-client; do
        [ -f "$ROOT/$part.pid" ] && kill "$(cat "$ROOT/$part.pid")" 2>/dev/null || true
        rm -f "${ROOT:?}/${part:?}.pid"
    done
    for i in $(seq 1 15); do
        pgrep -f "$BUILD/(domain-server|assignment-client)" > /dev/null || return 0
        sleep 1
    done
}

mkdir -p "$OUT"
python3 -I "$D/http-echo.py" 43110 "$D/../.." > /dev/null 2>&1 &
HTTP_PID=$!
trap 'kill $HTTP_PID 2>/dev/null' EXIT
WAS_RUNNING=0
[ -f "$ROOT/domain-server.pid" ] && kill -0 "$(cat "$ROOT/domain-server.pid")" 2>/dev/null && WAS_RUNNING=1
stop_sandbox
settings on
SANDBOX_ASSIGNMENTS=8 "$HOME/git/tivolicloud/domain-sandbox.sh" wasm "$BUILD" 43102 > /dev/null
LOG="$ROOT/logs/assignment-client.log"
for i in $(seq 1 90); do
    sleep 1
    # printed lines only: the log also quotes the script's source as it is downloaded
    grep "WALK-FRAMES RESULT" "$LOG" 2>/dev/null | grep -qv '\\n' && break
done
grep -h "WALK-FRAMES" "$LOG" | grep -v '\\n' | sed 's/.*WALK-FRAMES /WALK-FRAMES /' > "$OUT/lines.txt" || true
cp "$LOG" "$OUT/assignment-client.log" 2>/dev/null || true
stop_sandbox
settings off
if [ $WAS_RUNNING = 1 ]; then
    "$HOME/git/tivolicloud/domain-sandbox.sh" wasm "$BUILD" 43102 > /dev/null
fi
echo "== the lines:"
cat "$OUT/lines.txt"
grep -qE "RESULT (.* )?PASS" "$OUT/lines.txt"
