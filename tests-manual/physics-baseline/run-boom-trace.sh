#!/bin/zsh
# The frame trace's camera, boom and ride columns (boom-trace.js; team/performance/INSTRUMENTS-PLAN.md, item 7): one
# walled-off client, serverless on world.json, traced two seconds in third person against a wall; PASS when every
# column is there, the wall holds the boom in and is named, and the box under the avatar is the ride.
#   tests-manual/physics-baseline/run-boom-trace.sh [path/to/Darkride.app] [results dir]
#
# Copyright 2026 Darkride.
#
# Distributed under the MIT License.
set -e
D=$(cd "$(dirname "$0")" && pwd)
APP=${${1:-"$D/../../build/interface/Darkride.app"}:A}
R=${2:-"${TMPDIR:-/tmp}/boom-trace"}
BIN="$APP/Contents/MacOS/Darkride"
[ -x "$BIN" ] || { echo "no client at $BIN" >&2; exit 1; }
mkdir -p "$R"
{ echo "var BOOM_CONFIG = { results: \"$R\" };"; cat "$D/boom-trace.js"; } > "$R/boom-trace-test.js"
rm -f "$R/boom-trace.csv"
export HIFI_METAVERSE_URL=${HIFI_METAVERSE_URL:-https://vr.darkride.ai}
echo "client: $BIN"; echo "results: $R"
SESSION=${SESSION:-physics}
CLIENT_TIMEOUT=${CLIENT_TIMEOUT:-180}
. "$D/harness.sh"
client boom-trace --url "file://$D/world.json" --testScript "$R/boom-trace-test.js" quitWhenFinished --testResultsLocation "$R" || true
grep -a "BOOM-TRACE" "$R/boom-trace.log" | sed 's/.*BOOM-TRACE/BOOM-TRACE/' || { echo "no BOOM-TRACE lines in $R/boom-trace.log" >&2; exit 1; }
grep -a -q "BOOM-TRACE PASS" "$R/boom-trace.log"
