#!/bin/zsh
# An AI resident's body's network load (hearing-load.js: heard audio posted fire-and-forget, a JSON exchange with a
# reply handler, a look around) as a persistent agent in the wasm sandbox for <minutes>, the agent's assignment
# client's resident memory sampled every 30 s: what an agent left on for hours would do to its memory, sooner.
#   tests-manual/wasm-scripting/hearing-load.sh <minutes> [rate]
# <rate> is the request rate as a multiple of an AI resident's body's (about 3 heard-audio posts a second): 10 unless
# given, so that hours of her load take minutes; 1 for a run at her own rate.
# The scripts are served from this checkout on 127.0.0.1:43110 (http-echo.py, which also answers the posts). Writes
# results/hearing-load-<engine>/ (rss.txt: seconds and MB; load.txt: the agent's LOAD lines; the logs). A sandbox
# that was running before is started again, plain, at the end. DARKRIDE_JS_WASM in the environment picks the guest.
set -e
MINUTES=${1:?usage: hearing-load.sh <minutes> [rate]}
RATE=${2:-10}
D=$(cd "$(dirname "$0")" && pwd)
BUILD=$(cd "$D/../../build" && pwd)
ROOT=$HOME/tivoli-sandboxes/wasm
SERVE=http://127.0.0.1:43110/tests-manual/wasm-scripting
OUT=$D/results/hearing-load

settings() {
    python3 - "$ROOT/domain-config.json" "$1" "$SERVE/hearing-load.js" <<'PY'
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
    for part in domain-server assignment-client; do
        [ -f "$ROOT/$part.pid" ] && kill "$(cat "$ROOT/$part.pid")" 2>/dev/null || true
        rm -f "$ROOT/$part.pid"
    done
    for i in $(seq 1 15); do
        pgrep -f "$BUILD/(domain-server|assignment-client)" > /dev/null || return 0
        sleep 1
    done
}

agent_pid() {
    grep -o '\[[0-9]*\] \[agent\]' "$ROOT/logs/assignment-client.log" 2>/dev/null | tail -1 | tr -d '[]' | cut -d' ' -f1
}

mkdir -p "$D/results"
echo "$RATE" > "$D/results/hearing-load-rate.txt"
python3 -I "$D/http-echo.py" 43110 "$D/../.." > /dev/null 2>&1 &
HTTP_PID=$!
trap 'kill $HTTP_PID 2>/dev/null' EXIT
WAS_RUNNING=0
[ -f "$ROOT/domain-server.pid" ] && kill -0 "$(cat "$ROOT/domain-server.pid")" 2>/dev/null && WAS_RUNNING=1
settings on
SANDBOX_ASSIGNMENTS=7 "$HOME/git/tivolicloud/domain-sandbox.sh" wasm "$BUILD" 43102 > /dev/null
rm -rf "$OUT"
mkdir -p "$OUT"
: > "$OUT/rss.txt"
STARTED=$(date +%s)
END=$((STARTED + MINUTES * 60))
while [ "$(date +%s)" -lt $END ]; do
    sleep 30
    PID=$(agent_pid)
    RSS=$( [ -n "$PID" ] && ps -o rss= -p "$PID" 2>/dev/null | tr -d ' ' || true)
    echo "$(( $(date +%s) - STARTED )) ${PID:-none} $( [ -n "$RSS" ] && echo $((RSS / 1024)) || echo gone)" >> "$OUT/rss.txt"
done
cp "$ROOT/logs/assignment-client.log" "$OUT/" 2>/dev/null || true
stop_sandbox
settings off
if [ $WAS_RUNNING = 1 ]; then
    "$HOME/git/tivolicloud/domain-sandbox.sh" wasm "$BUILD" 43102 > /dev/null
fi
grep -h "LOAD " "$OUT/assignment-client.log" | grep -v '\\n' | sed 's/.*LOAD /LOAD /' > "$OUT/load.txt" || true
echo "== $MINUTES min at $RATE times her rate: seconds, agent pid, resident MB"
cat "$OUT/rss.txt"
echo "-- the agent's LOAD lines:"
cat "$OUT/load.txt"
