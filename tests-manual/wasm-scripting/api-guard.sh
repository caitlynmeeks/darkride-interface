#!/bin/sh
#
#  api-guard.sh
#
#  Created by scripting on 2026-10-30.
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#
# Runs api-guard.js in a serverless world on both engines, with http-echo.py serving this folder on 127.0.0.1:43110 and
# the probe data made in results/api-guard-data (a JSON file, a file holding a JavaScript expression, a good archive and
# one whose entry climbs out of its folder). Prints the script's lines and the app's refusal lines.
#   tests-manual/wasm-scripting/api-guard.sh <name>
set -e
D=$(cd "$(dirname "$0")" && pwd)
NAME=$1
[ -n "$NAME" ] || { echo "usage: $0 <name>" >&2; exit 1; }
DATA="$D/results/api-guard-data"
rm -rf "$DATA" && mkdir -p "$DATA"
printf '{"a":1}' > "$DATA/data.json"
printf '(print("API-GUARD: EVALUATED"), 42)' > "$DATA/code.json"
python3 -I - "$DATA" <<'PY'
import sys, zipfile
data = sys.argv[1]
with zipfile.ZipFile(data + "/good.zip", "w") as z:
    z.writestr("folder/readme.txt", "hello")
with zipfile.ZipFile(data + "/escape.zip", "w") as z:
    z.writestr("folder/readme.txt", "hello")
    z.writestr("../../escape.txt", "outside")
PY
python3 -I "$D/http-echo.py" 43110 "$D" > /dev/null 2>&1 &
ECHO_PID=$!
trap 'kill $ECHO_PID 2>/dev/null || true' EXIT
sleep 1
for ENGINE in wasm qtscript; do
    if [ "$ENGINE" = qtscript ]; then export DARKRIDE_RUN_JS_ON_WASM=0; else export DARKRIDE_RUN_JS_ON_WASM=1; fi
    "$D/run-client.sh" "$NAME-$ENGINE" 60 --url "file://$D/world.json" --testScript "$D/api-guard.js" || true
    LOG="$D/results/$NAME-$ENGINE/client.log"
    echo "== $NAME-$ENGINE: the script's lines"
    grep "API-GUARD" "$LOG" | sed 's/^.*\[hifi.scriptengine[^]]*\] //'
    echo "== $NAME-$ENGINE: the app's refusal lines"
    grep "Refused to unzip\|openUrl refused\|Unzip failed" "$LOG" | sed 's/^.*\] \[hifi[^]]*\] //' | cut -c1-200
done
ls -la /private/var/folders/*/*/T/escape.txt 2>/dev/null && echo "ESCAPE FILE EXISTS" || echo "no escape file in the temp folder"
