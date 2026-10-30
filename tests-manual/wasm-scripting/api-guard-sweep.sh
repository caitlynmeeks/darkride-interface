#!/bin/sh
#
#  api-guard-sweep.sh
#
#  Created by scripting on 2026-10-30.
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#
# The completeness check of the script API policy (ScriptApiPolicy): the live worlds' client entity scripts, copied into
# results/api-guard-sweep-scripts, run as entity scripts in the serverless world on both engines; every refusal line and
# script error in the logs is printed, so a content class or member the policy wrongly keeps out shows up here.
#   tests-manual/wasm-scripting/api-guard-sweep.sh <name>
set -e
D=$(cd "$(dirname "$0")" && pwd)
NAME=$1
[ -n "$NAME" ] || { echo "usage: $0 <name>" >&2; exit 1; }
ls "$D/results/api-guard-sweep-scripts/"*.js > /dev/null
for ENGINE in wasm qtscript; do
    if [ "$ENGINE" = qtscript ]; then export DARKRIDE_RUN_JS_ON_WASM=0; else export DARKRIDE_RUN_JS_ON_WASM=1; fi
    "$D/run-client.sh" "$NAME-$ENGINE" 60 --url "file://$D/world.json" --testScript "$D/api-guard-sweep.js" || true
    LOG="$D/results/$NAME-$ENGINE/client.log"
    echo "== $NAME-$ENGINE: the test script's lines"
    grep "API-GUARD-SWEEP" "$LOG" | sed 's/^.*\[hifi.scriptengine[^]]*\] //' | cut -c1-160
    echo "== $NAME-$ENGINE: refusals by the policy"
    grep -c "is for the visitor's own scripts" "$LOG" | sed 's/^/count: /'
    grep "is for the visitor's own scripts" "$LOG" | sed 's/^.*\] \[hifi[^]]*\] //' | sort | uniq -c | cut -c1-200 | head -30
    echo "== $NAME-$ENGINE: script errors (entity scripts)"
    grep -i "UncaughtException\|TypeError\|ReferenceError\|is not a function\|\[CRITICAL\]" "$LOG" | grep -v "API-GUARD" | sed 's/^.*\] \[hifi[^]]*\] //' | sort | uniq -c | cut -c1-200 | head -20
done
