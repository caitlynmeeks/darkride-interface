#!/bin/sh
# An entity's actionData on this build against the recorded wire format, through the client's own action classes
# (action-data-golden.js: every type written, the nine goldens read), in one walled serverless client:
#   tests-manual/wasm-scripting/action-data-golden.sh [timeout seconds, 60]
# CLIENT_APP=<another Darkride.app> runs that build instead (a Qt 6 client against the Qt 5 goldens is the point).
# Prints the ACTION-DATA lines; exits 0 when the RESULT line says PASS. The log: results/action-data-golden/client.log.
#
# Copyright 2026 Darkride.
# Distributed under the MIT License.
set -e
D=$(cd "$(dirname "$0")" && pwd)
SECONDS_MAX=${1:-60}
LOG="$D/results/action-data-golden/client.log"
"$D/run-client.sh" action-data-golden "$SECONDS_MAX" \
    --url "file://$D/world.json" --testScript "$D/action-data-golden.js" --testResultsLocation "$D/results/action-data-golden" \
    | tee "$D/results/action-data-golden.run.txt" || true
grep -o "ACTION-DATA .*" "$LOG" || echo "no ACTION-DATA lines"
grep -q "ACTION-DATA RESULT PASS" "$LOG"
