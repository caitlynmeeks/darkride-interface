#!/bin/zsh
# An entity's scripts turned off and on again (scriptsEnabled), in the wasm sandbox (ports 43100-43102), on either
# engine: scripts-off-driver.js as a persistent script (an agent) and a test client running scripts-off-client.js,
# which stands in the box the driver adds; the box's server entity script runs in the sandbox's entity script server,
# its client entity script in the test client. Prints the driver's SCRIPTS-OFF lines; exits 0 when its RESULT says
# PASS. A sandbox that was running before is started again, plain, at the end.
#   tests-manual/wasm-scripting/scripts-off.sh
# The driver is served from this checkout on 127.0.0.1:43110 (http-echo.py): an agent loads no script from the local
# file system.
#
# Copyright 2026 Darkride.
# Distributed under the MIT License.
set -e
D=$(cd "$(dirname "$0")" && pwd)
BUILD=$(cd "$D/../../build" && pwd)
ROOT=$HOME/tivoli-sandboxes/wasm
SERVE=http://127.0.0.1:43110/tests-manual/wasm-scripting
OUT=$D/results/scripts-off

settings() {
    python3 - "$ROOT/domain-config.json" "$1" "$SERVE/scripts-off-driver.js" <<'PY'
import json, sys
path, mode, driver = sys.argv[1:4]
with open(path) as f:
    config = json.load(f)
if mode == "on":
    config.setdefault("scripts", {})["persistent_scripts"] = [{"url": driver, "num_instances": 1, "pool": ""}]
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
    echo "servers of $BUILD still running after 15 s" >&2
}

python3 -I "$D/http-echo.py" 43110 "$D/../.." > /dev/null 2>&1 &
HTTP_PID=$!
trap 'kill $HTTP_PID 2>/dev/null' EXIT
WAS_RUNNING=0
[ -f "$ROOT/domain-server.pid" ] && kill -0 "$(cat "$ROOT/domain-server.pid")" 2>/dev/null && WAS_RUNNING=1
stop_sandbox
settings on
# the box an earlier run left in the sandbox's world (its deletion races the sandbox's stop): its scripts would start
# before the driver could delete it
python3 - "$ROOT/entities/models.json.gz" <<'PY'
import gzip, json, os, sys
path = sys.argv[1]
if os.path.exists(path):
    with gzip.open(path, "rt") as f:
        world = json.load(f)
    entities = world.get("Entities", [])
    kept = [e for e in entities if e.get("name") != "scripts-off box"]
    if len(kept) != len(entities):
        world["Entities"] = kept
        with gzip.open(path, "wt") as f:
            json.dump(world, f)
PY
SANDBOX_ASSIGNMENTS=7 "$HOME/git/tivolicloud/domain-sandbox.sh" wasm "$BUILD" 43102 > /dev/null
LOG="$ROOT/logs/assignment-client.log"
sleep 5
"$D/run-client.sh" scripts-off-client 110 --url hifi://localhost:43102 \
    --testScript "$D/scripts-off-client.js" > /dev/null 2>&1 &
CLIENT_RUN=$!
for i in $(seq 1 120); do
    sleep 1
    # printed lines only: the log also quotes the script's source as it is downloaded
    grep "SCRIPTS-OFF RESULT" "$LOG" 2>/dev/null | grep -qv '\\n' && break
done
wait $CLIENT_RUN || true
rm -rf "$OUT"
mkdir -p "$OUT"
grep -h "SCRIPTS-OFF" "$LOG" | grep -v '\\n' | sed 's/.*SCRIPTS-OFF /SCRIPTS-OFF /' > "$OUT/driver.txt" || true
cp "$LOG" "$OUT/assignment-client.log" 2>/dev/null || true
cp "$D/results/scripts-off-client/client.log" "$OUT/client.log" 2>/dev/null || true
rm -rf "$D/results/scripts-off-client"
stop_sandbox
settings off
if [ $WAS_RUNNING = 1 ]; then
    "$HOME/git/tivolicloud/domain-sandbox.sh" wasm "$BUILD" 43102 > /dev/null
fi
echo "== the lines:"
cat "$OUT/driver.txt"
grep -qE "RESULT PASS" "$OUT/driver.txt"
