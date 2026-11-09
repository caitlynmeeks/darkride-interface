#!/bin/sh
# A script's animation state handlers on this build (animation-handler.js: the checks (a)-(g) avatars asked for),
# in one walled serverless client:  tests-manual/wasm-scripting/animation-handler.sh [timeout seconds, 90]
# Prints the ANIM-HANDLER lines, the count of "Ignoring unrecognized data" lines (check (g): none expected) and the
# client's exit (check (f): a clean exit with a handler still added); exits 0 when the RESULT line says PASS, the
# count is 0 and the client exited 0. The log: results/animation-handler/client.log.
#
# Copyright 2026 Darkride.
# Distributed under the MIT License.
set -e
D=$(cd "$(dirname "$0")" && pwd)
SECONDS_MAX=${1:-90}
LOG="$D/results/animation-handler/client.log"
"$D/run-client.sh" animation-handler "$SECONDS_MAX" \
    --url "file://$D/world.json" --testScript "$D/animation-handler.js" --testResultsLocation "$D/results/animation-handler" \
    | tee "$D/results/animation-handler.run.txt" || true
grep -o "ANIM-HANDLER .*" "$LOG" || echo "no ANIM-HANDLER lines"
IGNORED=$(grep -c "Ignoring unrecognized data" "$LOG" || true)
echo "(g) 'Ignoring unrecognized data' lines: $IGNORED"
EXIT=$(grep -o "exited with status [0-9]*" "$D/results/animation-handler.run.txt" | tail -1)
echo "(f) client $EXIT"
grep -q "ANIM-HANDLER RESULT PASS" "$LOG" && [ "$IGNORED" = 0 ] && [ "$EXIT" = "exited with status 0" ]
