#!/bin/zsh
# The kart at speed from its chase camera (judder.js): one walled-off client with the engine's frame trace on
# (Test.frameTrace; the camera and the ride per frame, cam.* and ride.* from ViewState.cpp, dumped per phase into
# frame-trace-<phase>.csv), serverless on world.json or on a domain whose sandbox is already up (not started here).
# Prints the JUDDER lines and judder-report.py's summary.
#   tests-manual/physics-baseline/run-judder.sh [path/to/Darkride.app] [udp port|serverless] [results dir]
#   JUDDER_CAMERA=entity ...           (the camera fixed in the kart's frame; "chase", the engine's chase camera, 1.1;
#                                       "script", vehicles.js's chase camera of 1.0, by default)
#   KART_RIG=path/to/kart-rig.js ...   (another kart's numbers than this tree's kart-rig.js)
#
# Copyright 2026 Darkride.
#
# Distributed under the MIT License.
set -e
D=$(cd "$(dirname "$0")" && pwd)
APP=${${1:-"$D/../../build/interface/Darkride.app"}:A}
PORT=${2:-serverless}
CAMERA=${JUDDER_CAMERA:-script}
R=${3:-"${TMPDIR:-/tmp}/judder-$PORT-$CAMERA"}
BIN="$APP/Contents/MacOS/Darkride"
[ -x "$BIN" ] || { echo "no client at $BIN" >&2; exit 1; }
mkdir -p "$R"   # Test.saveObject writes into the results location only when it exists
SERVERLESS=false
[ "$PORT" = "serverless" ] && SERVERLESS=true
{ echo "var JUDDER_CONFIG = { camera: \"$CAMERA\", serverless: $SERVERLESS, results: \"$R\" };"
  cat "${KART_RIG:-$D/kart-rig.js}" "$D/judder.js"; } > "$R/judder-test.js"
rm -f "$R"/frame-trace-*.csv
export HIFI_METAVERSE_URL=${HIFI_METAVERSE_URL:-https://vr.darkride.ai}
URL="hifi://localhost:$PORT"
[ "$PORT" = "serverless" ] && URL="file://$D/world.json"
echo "client: $BIN"; echo "world: $URL"; echo "camera: $CAMERA"; echo "results: $R"
SESSION=${SESSION:-physics}
. "$D/harness.sh"
client judder --url "$URL" --testScript "$R/judder-test.js" quitWhenFinished --testResultsLocation "$R" || true
grep -a "JUDDER" "$R/judder.log" | sed 's/.*JUDDER/JUDDER/' || { echo "no JUDDER lines in $R/judder.log" >&2; exit 1; }
python3 "$D/judder-report.py" "$R"
