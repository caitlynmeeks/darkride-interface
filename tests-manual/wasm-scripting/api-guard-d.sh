#!/bin/sh
#
#  api-guard-d.sh
#
#  Created by scripting on 2026-10-30.
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#
# Runs api-guard-d.js in the serverless world on both engines: it adds an entity carrying api-guard-entity.js, which
# runs on the world's engine and probes the script API policy (ScriptApiPolicy). Prints the entity script's lines and
# the app's refusal lines.
#   tests-manual/wasm-scripting/api-guard-d.sh <name>
set -e
D=$(cd "$(dirname "$0")" && pwd)
NAME=$1
[ -n "$NAME" ] || { echo "usage: $0 <name>" >&2; exit 1; }
for ENGINE in wasm qtscript; do
    if [ "$ENGINE" = qtscript ]; then export DARKRIDE_RUN_JS_ON_WASM=0; else export DARKRIDE_RUN_JS_ON_WASM=1; fi
    "$D/run-client.sh" "$NAME-$ENGINE" 60 --url "file://$D/world.json" --testScript "$D/api-guard-d.js" || true
    LOG="$D/results/$NAME-$ENGINE/client.log"
    echo "== $NAME-$ENGINE: the scripts' lines"
    grep "API-GUARD" "$LOG" | sed 's/^.*\[hifi.scriptengine[^]]*\] //' | cut -c1-200
    echo "== $NAME-$ENGINE: the app's refusal lines"
    grep -c "is for the visitor's own scripts" "$LOG" | sed 's/^/count: /'
    grep "is for the visitor's own scripts" "$LOG" | sed 's/^.*\] \[hifi[^]]*\] //' | cut -c1-160 | head -12
done
