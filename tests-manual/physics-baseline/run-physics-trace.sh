#!/bin/zsh
# The frame trace's physics columns (physics-trace.js; team/performance/INSTRUMENTS-PLAN.md, item 4): one
# walled-off client, serverless on world.json, traced two seconds standing; PASS when the three columns are in every
# frame, in range, and the substeps add up to the simulation's 90 a second.
#   tests-manual/physics-baseline/run-physics-trace.sh [path/to/Darkride.app] [results dir]
#
# Copyright 2026 Darkride.
#
# Distributed under the MIT License.
set -e
D=$(cd "$(dirname "$0")" && pwd)
APP=${${1:-"$D/../../build/interface/Darkride.app"}:A}
R=${2:-"${TMPDIR:-/tmp}/physics-trace"}
BIN="$APP/Contents/MacOS/Darkride"
[ -x "$BIN" ] || { echo "no client at $BIN" >&2; exit 1; }
mkdir -p "$R"
{ echo "var PHYSICS_CONFIG = { results: \"$R\" };"; cat "$D/physics-trace.js"; } > "$R/physics-trace-test.js"
rm -f "$R/physics-trace.csv"
export HIFI_METAVERSE_URL=${HIFI_METAVERSE_URL:-https://vr.darkride.ai}
echo "client: $BIN"; echo "results: $R"
SESSION=${SESSION:-physics}
CLIENT_TIMEOUT=${CLIENT_TIMEOUT:-180}
. "$D/harness.sh"
client physics-trace --url "file://$D/world.json" --testScript "$R/physics-trace-test.js" quitWhenFinished --testResultsLocation "$R" || true
grep -a "PHYSICS-TRACE" "$R/physics-trace.log" | sed 's/.*PHYSICS-TRACE/PHYSICS-TRACE/' || { echo "no PHYSICS-TRACE lines in $R/physics-trace.log" >&2; exit 1; }
grep -a -q "PHYSICS-TRACE PASS" "$R/physics-trace.log"
