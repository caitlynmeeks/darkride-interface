#!/bin/sh
#
#  network-rules.sh
#
#  Created by scripting on 2026-10-30.
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#
# Runs network-rules.js in a serverless world, with http-echo.py answering on 127.0.0.1:43110, and prints its lines
# and the engine's refusal lines from the log.
#   tests-manual/wasm-scripting/network-rules.sh <name> [strict]
# <name> names the results folder (results/<name>); "strict" sets DARKRIDE_SCRIPT_NETWORK_STRICT=1, which holds the
# local test script to the rule as a script from the network.
set -e
D=$(cd "$(dirname "$0")" && pwd)
NAME=$1
[ -n "$NAME" ] || { echo "usage: $0 <name> [strict]" >&2; exit 1; }
if [ "$2" = "strict" ]; then
    export DARKRIDE_SCRIPT_NETWORK_STRICT=1
fi
python3 -I "$D/http-echo.py" 43110 "$D" > /dev/null 2>&1 &    # serves this folder: world.json, network-rules-included.js
ECHO_PID=$!
trap 'kill $ECHO_PID 2>/dev/null || true' EXIT
sleep 1
"$D/run-client.sh" "$NAME" 90 --url "file://$D/world.json" --testScript "$D/network-rules.js" || true
LOG="$D/results/$NAME/client.log"
echo "== $NAME: the script's lines"
grep "NETWORK-RULES" "$LOG" | sed 's/^.*\[hifi.scriptengine[^]]*\] //'
echo "== $NAME: the engine's refusal lines"
grep -c "may not reach" "$LOG" | sed 's/^/count: /'
grep "may not reach" "$LOG" | sed 's/^.*\[hifi.scriptengine\] //' | cut -c1-200 | head -20
