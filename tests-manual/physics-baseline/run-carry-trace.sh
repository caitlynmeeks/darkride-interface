#!/bin/zsh
# The frame trace's carry columns (carry-trace.js; team/performance/INSTRUMENTS-PLAN.md, item 5): one walled-off
# client, serverless on world.json, carried standing on a moving frame for two seconds; PASS when the carry columns
# are there, the frame is named, the local step stays under 2 mm a frame and the lead is written.
#   tests-manual/physics-baseline/run-carry-trace.sh [path/to/Darkride.app] [results dir]
#
# Copyright 2026 Darkride.
#
# Distributed under the MIT License.
set -e
D=$(cd "$(dirname "$0")" && pwd)
APP=${${1:-"$D/../../build/interface/Darkride.app"}:A}
R=${2:-"${TMPDIR:-/tmp}/carry-trace"}
BIN="$APP/Contents/MacOS/Darkride"
[ -x "$BIN" ] || { echo "no client at $BIN" >&2; exit 1; }
mkdir -p "$R"
{ echo "var CARRY_CONFIG = { results: \"$R\" };"; cat "$D/carry-trace.js"; } > "$R/carry-trace-test.js"
rm -f "$R/carry-trace.csv"
export HIFI_METAVERSE_URL=${HIFI_METAVERSE_URL:-https://vr.darkride.ai}
echo "client: $BIN"; echo "results: $R"
SESSION=${SESSION:-physics}
CLIENT_TIMEOUT=${CLIENT_TIMEOUT:-180}
. "$D/harness.sh"
client carry-trace --url "file://$D/world.json" --testScript "$R/carry-trace-test.js" quitWhenFinished --testResultsLocation "$R" || true
grep -a "CARRY-TRACE" "$R/carry-trace.log" | sed 's/.*CARRY-TRACE/CARRY-TRACE/' || { echo "no CARRY-TRACE lines in $R/carry-trace.log" >&2; exit 1; }
grep -a -q "CARRY-TRACE PASS" "$R/carry-trace.log"
