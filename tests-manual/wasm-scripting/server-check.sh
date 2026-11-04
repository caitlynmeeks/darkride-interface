#!/bin/zsh
# The entity script server, the entity server's edit filter and an agent on one engine, in the wasm sandbox
# (~/git/tivolicloud/domain-sandbox.sh's, ports 43100-43102): sets the sandbox's settings (agent-check.js as a
# persistent script, server-edit-filter.js as the entity edit filter, both served over HTTP from this checkout on
# 127.0.0.1:43110, since an agent and the entity server load no script from the local file system; and localhost
# without the right to change locks, which would let the agent's edits pass the filter unseen), starts it from
# this checkout's build with one assignment client more than the six servers, waits, copies the new logs to
# results/server-check/,
# prints the AGENT-CHECK and SERVER-CHECK lines and the CRITICAL ones, stops the sandbox and takes the settings out
# again; a sandbox that was running before is started again as it was.
#   tests-manual/wasm-scripting/server-check.sh [seconds, 70]
# SERVER_CHECK_BUILD names another checkout's build to run (the servers and the client), such as a hands-on build.
set -e
SECONDS_RUN=${1:-70}
D=$(cd "$(dirname "$0")" && pwd)
BUILD=$(cd "${SERVER_CHECK_BUILD:-$D/../../build}" && pwd)
ROOT=$HOME/tivoli-sandboxes/wasm
SERVE=http://127.0.0.1:43110/tests-manual/wasm-scripting
OUT=$D/results/server-check

settings() {
    python3 - "$ROOT/domain-config.json" "$1" "$SERVE/agent-check.js" "$SERVE/server-edit-filter.js" <<'PY'
import json, sys
path, mode, agent, filter_url = sys.argv[1:5]
with open(path) as f:
    config = json.load(f)
ALL = ["id_can_connect", "id_can_connect_past_max_capacity", "id_can_kick", "id_can_replace_content", "id_can_rez",
       "id_can_rez_certified", "id_can_rez_tmp", "id_can_rez_tmp_certified", "id_can_write_to_asset_server",
       "id_can_get_and_set_private_user_data"]
if mode == "on":
    config.setdefault("scripts", {})["persistent_scripts"] = [{"url": agent, "num_instances": 1, "pool": ""}]
    config.setdefault("entity_server_settings", {})["entityEditFilter"] = filter_url
    # the domain-server's default rows, but localhost may not change locks (a lock changer bypasses the filter)
    localhost = dict((key, True) for key in ALL)
    localhost.update({"permissions_id": "localhost", "id_can_adjust_locks": False})
    config.setdefault("security", {})["standard_permissions"] = [
        {"id_can_connect": True, "id_can_rez_tmp_certified": True, "permissions_id": "anonymous"},
        {"id_can_connect": True, "id_can_rez_tmp_certified": True, "permissions_id": "friends"},
        localhost,
        {"id_can_connect": True, "id_can_rez_tmp_certified": True, "permissions_id": "logged-in"}]
else:
    for group, key in (("scripts", "persistent_scripts"), ("entity_server_settings", "entityEditFilter"),
                       ("security", "standard_permissions")):
        config.get(group, {}).pop(key, None)
        if config.get(group) == {}:
            config.pop(group)
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
# whether the sandbox runs now, to start it again plain at the end
WAS_RUNNING=0
[ -f "$ROOT/domain-server.pid" ] && kill -0 "$(cat "$ROOT/domain-server.pid")" 2>/dev/null && WAS_RUNNING=1
settings on
# the entities an earlier run left in the sandbox's world: its entity script server would start them before the agent
# could delete them
python3 - "$ROOT/entities/models.json.gz" <<'PY'
import gzip, json, os, sys
path = sys.argv[1]
if os.path.exists(path):
    with gzip.open(path, "rt") as f:
        world = json.load(f)
    entities = world.get("Entities", [])
    kept = [e for e in entities if not (str(e.get("name", "")).startswith("server-check") or e.get("name") == "forbidden")]
    if len(kept) != len(entities):
        world["Entities"] = kept
        with gzip.open(path, "wt") as f:
            json.dump(world, f)
        print("removed %d entities of an earlier run from the sandbox's world" % (len(entities) - len(kept)))
PY
MARK=$(mktemp)
SANDBOX_ASSIGNMENTS=7 "$HOME/git/tivolicloud/domain-sandbox.sh" wasm "$BUILD" 43102
# a client without the right to change locks edits through the filter
sleep 5
CLIENT_APP="$BUILD/interface/Darkride.app" "$D/run-client.sh" server-check-filter 65 --url hifi://localhost:43102 \
    --testScript "$D/filter-client.js" > /dev/null 2>&1 &
CLIENT_RUN=$!
sleep $((SECONDS_RUN - 5))
wait $CLIENT_RUN || true
rm -rf "$OUT"
mkdir -p "$OUT"
find "$ROOT/logs" -type f -newer "$MARK" -exec cp {} "$OUT/" \;
rm -f "$MARK"
stop_sandbox
settings off
if [ $WAS_RUNNING = 1 ]; then
    "$HOME/git/tivolicloud/domain-sandbox.sh" wasm "$BUILD" 43102 > /dev/null
fi
cp "$D/results/server-check-filter/client.log" "$OUT/filter-client.log" 2>/dev/null || true
rm -rf "$D/results/server-check-filter/data/ktx_cache"
cd "$OUT"
# the printed lines only, not the script's own text where a log quotes it
grep -h "AGENT-CHECK" *.txt *.log 2>/dev/null | grep -v '\\n' | sed 's/.*AGENT-CHECK/AGENT-CHECK/' > agent-check.txt || true
grep -h "SERVER-CHECK" *.txt *.log 2>/dev/null | grep -v '\\n' | sed 's/.*SERVER-CHECK/SERVER-CHECK/' > server-check.txt || true
grep -h "FILTER-CHECK" filter-client.log 2>/dev/null | grep -v '\\n' | sed 's/.*FILTER-CHECK/FILTER-CHECK/' >> server-check.txt || true
grep -h "\[CRITICAL\]\|longer than its limit\|is stopped" *.txt *.log 2>/dev/null | sed 's/^\[[^]]*\] //' | sort | uniq -c > critical.txt || true
echo "== $(wc -l < agent-check.txt | tr -d ' ') AGENT-CHECK lines, $(wc -l < server-check.txt | tr -d ' ') SERVER-CHECK lines; logs in $OUT"
cat agent-check.txt server-check.txt
echo "-- CRITICAL and limit lines (count, text):"
cut -c1-220 critical.txt
