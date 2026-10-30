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
#   [BEFORE_APP=<another Darkride.app>] tests-manual/wasm-scripting/api-guard.sh <name>
# BEFORE_APP runs the same probe first on a client built without the guards (the before/after of the archive check).
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
with zipfile.ZipFile(data + "/links.zip", "w") as z:
    link = zipfile.ZipInfo("link")
    link.external_attr = (0o120777 << 16)      # a symlink entry, target /etc
    z.writestr(link, "/etc")
    z.writestr("link/x", "chained")
    z.writestr("plain.txt", "plain")
with zipfile.ZipFile(data + "/link-only.zip", "w") as z:
    link = zipfile.ZipInfo("link")
    link.external_attr = (0o120777 << 16)      # the symlink entry alone, so the extraction completes and the file can be inspected
    z.writestr(link, "/etc")
    z.writestr("plain.txt", "plain")
PY
python3 -I "$D/http-echo.py" 43110 "$D" > /dev/null 2>&1 &
ECHO_PID=$!
trap 'kill $ECHO_PID 2>/dev/null || true' EXIT
sleep 1
rm -f /private/var/folders/*/*/T/escape.txt 2>/dev/null
RUNS="wasm qtscript"
[ -n "$BEFORE_APP" ] && RUNS="before $RUNS"    # BEFORE_APP=<another Darkride.app without the guards>: the same probe on it first
for ENGINE in $RUNS; do
    unset CLIENT_APP
    if [ "$ENGINE" = qtscript ]; then export DARKRIDE_RUN_JS_ON_WASM=0; else export DARKRIDE_RUN_JS_ON_WASM=1; fi
    [ "$ENGINE" = before ] && export CLIENT_APP="$BEFORE_APP"
    "$D/run-client.sh" "$NAME-$ENGINE" 60 --url "file://$D/world.json" --testScript "$D/api-guard.js" || true
    LOG="$D/results/$NAME-$ENGINE/client.log"
    echo "== $NAME-$ENGINE: the script's lines"
    grep "API-GUARD" "$LOG" | sed 's/^.*\[hifi.scriptengine[^]]*\] //'
    echo "== $NAME-$ENGINE: the app's refusal lines"
    grep "Refused to unzip\|openUrl refused\|Unzip failed" "$LOG" | sed 's/^.*\] \[hifi[^]]*\] //' | cut -c1-200
    echo "== $NAME-$ENGINE: what the archives left on disk"
    ls -la /private/var/folders/*/*/T/escape.txt 2>/dev/null && echo "ESCAPE FILE EXISTS (written outside the target)" || echo "no escape file outside the target"
    rm -f /private/var/folders/*/*/T/escape.txt 2>/dev/null
    for T in $(grep "Temporary directory at" "$LOG" | sed 's/^.*Temporary directory at: //' | tr -d '"'); do
        [ -e "$T/model_repo/link" ] || continue
        if [ -L "$T/model_repo/link" ]; then echo "link: A SYMLINK -> $(readlink "$T/model_repo/link")"; else echo "link: a plain file holding '$(cat "$T/model_repo/link")', not a link"; fi
        [ -e "$T/model_repo/link/x" ] && echo "link/x: exists" || echo "link/x: not written"
    done
done
