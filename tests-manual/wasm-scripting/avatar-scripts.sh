#!/bin/sh
# An avatar's scripts on this build (security finding #66): a stand-in avatar (avatar-scripts/av/probe.fst, served by
# this kit's own web server on localtest.me:43110) is worn in one walled serverless client. Its script next to the
# .fst runs under a world's rules (probe.js prints the checks); its three script lines that point outside the avatar's
# folder are refused and never run. Then it is taken off (plain.fst, the same avatar without script lines, made here): its
# world is left before its script is stopped, so what the script's scriptEnding handler then tries is refused.
#   tests-manual/wasm-scripting/avatar-scripts.sh [timeout seconds, 90]
# CLIENT_APP=<another Darkride.app> runs that build instead (a build before the rule shows the FAIL lines).
# MIDCALL=1 runs the mid-call check instead (avatar-scripts-midcall.js): the avatar's script stopped while inside a
# call, by taking the avatar off (ends at once), by a plain stop during a long call (ends at the 3 s deadline) and
# during a short one (finishes); exits 0 when all three PASS and only the short call's spin ended.
# Prints the AVATAR-SCRIPT lines and the two counts; exits 0 when the avatar's RESULT says PASS, the avatar stayed
# the probe, three outside script lines were refused and none ran, and taking it off stopped its script with its late call
# refused. The log: results/avatar-scripts/client.log.
#
# Copyright 2026 Darkride.
# Distributed under the MIT License.
set -e
D=$(cd "$(dirname "$0")" && pwd)
SECONDS_MAX=${1:-90}
LOG="$D/results/avatar-scripts/client.log"
DRIVER=avatar-scripts.js
[ -n "$MIDCALL" ] && DRIVER=avatar-scripts-midcall.js
mkdir -p "$D/results/avatar-scripts"
sed -e '/^script = /d' -e 's/^name = probe/name = plain/' "$D/avatar-scripts/av/probe.fst" > "$D/avatar-scripts/av/plain.fst"
python3 -m http.server 43110 --bind 127.0.0.1 --directory "$D/avatar-scripts" > "$D/results/avatar-scripts.server.txt" 2>&1 &
SERVER=$!
sleep 1
"$D/run-client.sh" avatar-scripts "$SECONDS_MAX" \
    --url "file://$D/world.json" --testScript "$D/$DRIVER" --testResultsLocation "$D/results/avatar-scripts" \
    | tee "$D/results/avatar-scripts.run.txt" || true
kill $SERVER 2>/dev/null || true
rm -f "$D/avatar-scripts/av/plain.fst"
grep -o "AVATAR-SCRIPT .*" "$LOG" || echo "no AVATAR-SCRIPT lines"
if [ -n "$MIDCALL" ]; then
    # each case, from the driver's "spin sent" to the next: the probe was spinning before the stop (or the stop found
    # it idle and proves nothing), and how many of its spins ended (takeoff 0, plain-stop 0, quick-stop 1)
    awk '
        /AVATAR-SCRIPT DRIVER [a-z-]+: spin [0-9]+ ms sent/ {
            match($0, /DRIVER [a-z-]+:/); c = substr($0, RSTART + 7, RLENGTH - 8); cases[++n] = c; cur = c; next }
        /AVATAR-SCRIPT spinning [0-9]+ ms/ { if (cur != "" && !stopping[cur]) spun[cur] = 1; next }
        /AVATAR-SCRIPT DRIVER [a-z-]+: stopping it mid-call/ { if (cur != "") stopping[cur] = 1; next }
        /AVATAR-SCRIPT spin ended/ { if (cur != "") ended[cur]++; next }
        END {
            want["takeoff"] = 0; want["plain-stop"] = 0; want["quick-stop"] = 1; ok = (n == 3)
            for (i = 1; i <= n; i++) {
                c = cases[i]; good = spun[c] && stopping[c] && (ended[c] + 0) == want[c]; ok = ok && good
                print "case " c ": spinning before the stop " (spun[c] ? "yes" : "NO") ", spins ended " (ended[c] + 0) \
                      " (expect " want[c] ") " (good ? "PASS" : "FAIL")
            }
            print "mid-call cases " (ok ? "PASS" : "FAIL")
        }' "$LOG" | tee "$D/results/avatar-scripts.cases.txt"
    grep -o "the call running at the stop of .*" "$LOG" || true
    grep -q "AVATAR-SCRIPT DRIVER takeoff-stopped after [0-9]* ms PASS" "$LOG" \
        && grep -q "AVATAR-SCRIPT DRIVER plain-stop-stopped after [0-9]* ms PASS" "$LOG" \
        && grep -q "AVATAR-SCRIPT DRIVER quick-stop-stopped after [0-9]* ms PASS" "$LOG" \
        && grep -q "^mid-call cases PASS" "$D/results/avatar-scripts.cases.txt"
    exit $?
fi
REFUSED=$(grep -c "is outside the avatar's own folder" "$LOG" || true)
RAN=$(grep -c "ELSEWHERE RAN" "$LOG" || true)
FETCHED=$(grep -c "elsewhere" "$D/results/avatar-scripts.server.txt" || true)
echo "script lines outside the avatar's folder: refused $REFUSED (expect 3), ran $RAN (expect 0), asked of the server $FETCHED (expect 0)"
LEFT=$(grep -c "addEntity: a script of a world that was left" "$LOG" || true)
echo "taken off: late calls refused as a world left $LEFT (expect 1)"
grep -q "AVATAR-SCRIPT RESULT PASS" "$LOG" && grep -q "AVATAR-SCRIPT DRIVER avatar-unchanged PASS" "$LOG" \
    && grep -q "AVATAR-SCRIPT DRIVER avatar-off PASS" "$LOG" && grep -q "AVATAR-SCRIPT DRIVER script-stopped PASS" "$LOG" \
    && grep -q "late entity refused" "$LOG" && [ "$LEFT" = 1 ] \
    && [ "$REFUSED" = 3 ] && [ "$RAN" = 0 ] && [ "$FETCHED" = 0 ]
