#!/bin/zsh
# The frame trace's server-state correction columns (correction-trace.js; team/performance/INSTRUMENTS-PLAN.md,
# item 6): one walled-off client on a domain whose sandbox is already up with a track car driven by the physics server
# (Port Amarelle on physics' port, or run-monorail.sh's world); it boards the car and rides eight seconds traced.
# PASS when the net.* columns are there, a state is adopted at least every 2.5 s, no residual is over 50 cm.
#   tests-manual/physics-baseline/run-correction-trace.sh [path/to/Darkride.app] [udp port] [results dir]
#
# Copyright 2026 Darkride.
#
# Distributed under the MIT License.
set -e
D=$(cd "$(dirname "$0")" && pwd)
APP=${${1:-"$D/../../build/interface/Darkride.app"}:A}
PORT=${2:-42102}
R=${3:-"${TMPDIR:-/tmp}/correction-trace-$PORT"}
BIN="$APP/Contents/MacOS/Darkride"
[ -x "$BIN" ] || { echo "no client at $BIN" >&2; exit 1; }
mkdir -p "$R"
{ echo "var CORRECTION_CONFIG = { results: \"$R\" };"; cat "$D/correction-trace.js"; } > "$R/correction-trace-test.js"
rm -f "$R/correction-trace.csv"
export HIFI_METAVERSE_URL=${HIFI_METAVERSE_URL:-https://vr.darkride.ai}
echo "client: $BIN"; echo "domain: hifi://localhost:$PORT"; echo "results: $R"
SESSION=${SESSION:-physics}
CLIENT_TIMEOUT=${CLIENT_TIMEOUT:-300}
. "$D/harness.sh"
client correction-trace --url "hifi://localhost:$PORT" --testScript "$R/correction-trace-test.js" quitWhenFinished --testResultsLocation "$R" || true
grep -a "CORRECTION-TRACE" "$R/correction-trace.log" | sed 's/.*CORRECTION-TRACE/CORRECTION-TRACE/' || { echo "no CORRECTION-TRACE lines in $R/correction-trace.log" >&2; exit 1; }
grep -a -q "CORRECTION-TRACE PASS" "$R/correction-trace.log"
