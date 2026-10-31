#!/bin/sh
# Runs the one-line test of Stats values live without the panel (stats-live.js; the instruments plan's item 8): one
# scripted client through run-client.sh (the machine lock, the test window's shape, 40 s limit), then the STATS-LIVE
# lines of its log. Usage: tests-manual/wasm-scripting/stats-live.sh [name]   (default stats-live-wasm)
# The verdict is the "STATS-LIVE: PASS" line; the log is results/<name>/client.log.
set -e
D=$(cd "$(dirname "$0")" && pwd)
NAME=${1:-stats-live-wasm}
URL_ARG=""
[ -f "$D/world.json" ] && URL_ARG="--url file://$D/world.json"
"$D/run-client.sh" "$NAME" 40 $URL_ARG --testScript "$D/stats-live.js" > /dev/null
echo "--- $NAME"
grep -E "STATS-LIVE|No such method" "$D/results/$NAME/client.log" || echo "no STATS-LIVE lines in the log"
