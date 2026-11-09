#!/bin/sh
# Runs parity.js on the script engine (WASM-SCRIPTING.md round 3) and diffs its PARITY lines against two recordings
# under results/qtscript-out-golden/: parity-wasm.txt (this engine before QtScript was removed; no difference expected)
# and parity-qtscript.txt (what the QtScript engine gave for the same script; the known differences):
#   tests-manual/wasm-scripting/parity.sh [timeout seconds]
# The client is this checkout's build, started through run-client.sh (with a time limit); the log is
# results/parity-wasm/client.log, the PARITY lines results/parity/wasm.txt.
set -e
D=$(cd "$(dirname "$0")" && pwd)
SECONDS_MAX=${1:-120}
R="$D/results/parity"
G="$D/results/qtscript-out-golden"
mkdir -p "$R"
"$D/run-client.sh" parity-wasm "$SECONDS_MAX" \
    --url "file://$D/world.json" --testScript "$D/parity.js" --testResultsLocation "$D/results/parity-wasm"
grep -o "PARITY .*" "$D/results/parity-wasm/client.log" > "$R/wasm.txt" || true
echo "wasm: $(wc -l < "$R/wasm.txt") lines"
echo "--- differences from the engine's own recording (< recorded, > this build; none expected):"
diff "$G/parity-wasm.txt" "$R/wasm.txt" || true
echo "--- differences from the QtScript engine's recording (< QtScript, > this build; the known ones):"
diff "$G/parity-qtscript.txt" "$R/wasm.txt" || true
