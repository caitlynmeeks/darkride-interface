#!/bin/sh
# An avatar's scripts on this build (security finding #66): a stand-in avatar (avatar-scripts/av/probe.fst, served by
# this kit's own web server on localtest.me:43110) is worn in one walled serverless client. Its script next to the
# .fst runs under a world's rules (probe.js prints the checks); its two script lines that point outside the avatar's
# folder are refused and never run.
#   tests-manual/wasm-scripting/avatar-scripts.sh [timeout seconds, 90]
# CLIENT_APP=<another Darkride.app> runs that build instead (a build before the rule shows the FAIL lines).
# Prints the AVATAR-SCRIPT lines and the two counts; exits 0 when the avatar's RESULT says PASS, the avatar stayed
# the probe, two outside script lines were refused and none ran. The log: results/avatar-scripts/client.log.
#
# Copyright 2026 Darkride.
# Distributed under the MIT License.
set -e
D=$(cd "$(dirname "$0")" && pwd)
SECONDS_MAX=${1:-90}
LOG="$D/results/avatar-scripts/client.log"
mkdir -p "$D/results/avatar-scripts"
python3 -m http.server 43110 --bind 127.0.0.1 --directory "$D/avatar-scripts" > "$D/results/avatar-scripts.server.txt" 2>&1 &
SERVER=$!
sleep 1
"$D/run-client.sh" avatar-scripts "$SECONDS_MAX" \
    --url "file://$D/world.json" --testScript "$D/avatar-scripts.js" --testResultsLocation "$D/results/avatar-scripts" \
    | tee "$D/results/avatar-scripts.run.txt" || true
kill $SERVER 2>/dev/null || true
grep -o "AVATAR-SCRIPT .*" "$LOG" || echo "no AVATAR-SCRIPT lines"
REFUSED=$(grep -c "is outside the avatar's own folder" "$LOG" || true)
RAN=$(grep -c "ELSEWHERE RAN" "$LOG" || true)
FETCHED=$(grep -c "elsewhere" "$D/results/avatar-scripts.server.txt" || true)
echo "script lines outside the avatar's folder: refused $REFUSED (expect 2), ran $RAN (expect 0), asked of the server $FETCHED (expect 0)"
grep -q "AVATAR-SCRIPT RESULT PASS" "$LOG" && grep -q "AVATAR-SCRIPT DRIVER avatar-unchanged PASS" "$LOG" \
    && [ "$REFUSED" = 2 ] && [ "$RAN" = 0 ] && [ "$FETCHED" = 0 ]
