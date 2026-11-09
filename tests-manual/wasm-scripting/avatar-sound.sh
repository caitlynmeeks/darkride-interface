#!/bin/zsh
# An AI resident's recorded lines (avatar-sound.js: Agent.avatarSoundFinished and Agent.stopAvatarSound) in a persistent
# agent in the wasm sandbox, on either engine, up to 90 s.
#   tests-manual/wasm-scripting/avatar-sound.sh
# The lines are tones made here (results/avatar-sound/tone-1s.wav, tone-3s.wav), served from this checkout with the
# script on 127.0.0.1:43110 (http-echo.py). Prints the agent's AVATAR-SOUND lines; exits 0 when its RESULT says PASS. A
# sandbox that was running before is started again, plain, at the end.
#
# Copyright 2026 Darkride.
# Distributed under the MIT License.
set -e
D=$(cd "$(dirname "$0")" && pwd)
BUILD=$(cd "$D/../../build" && pwd)
ROOT=$HOME/tivoli-sandboxes/wasm
SERVE=http://127.0.0.1:43110/tests-manual/wasm-scripting
OUT=$D/results/avatar-sound

settings() {
    python3 - "$ROOT/domain-config.json" "$1" "$SERVE/avatar-sound.js" <<'PY'
import json, sys
path, mode, agent = sys.argv[1:4]
with open(path) as f:
    config = json.load(f)
if mode == "on":
    config.setdefault("scripts", {})["persistent_scripts"] = [{"url": agent, "num_instances": 1, "pool": ""}]
else:
    config.get("scripts", {}).pop("persistent_scripts", None)
    if config.get("scripts") == {}:
        config.pop("scripts")
with open(path, "w") as f:
    json.dump(config, f, indent=2)
PY
}

stop_sandbox() {
    for part in domain-server assignment-client; do
        [ -f "$ROOT/$part.pid" ] && kill "$(cat "$ROOT/$part.pid")" 2>/dev/null || true
        rm -f "${ROOT:?}/${part:?}.pid"
    done
    for i in $(seq 1 15); do
        pgrep -f "$BUILD/(domain-server|assignment-client)" > /dev/null || return 0
        sleep 1
    done
}

mkdir -p "$OUT"
python3 - "$OUT" <<'PY'
import math, struct, sys, wave
for seconds in (1, 3):
    with wave.open("%s/tone-%ds.wav" % (sys.argv[1], seconds), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(24000)
        w.writeframes(b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 440 * i / 24000)))
                               for i in range(24000 * seconds)))
PY
python3 -I "$D/http-echo.py" 43110 "$D/../.." > /dev/null 2>&1 &
HTTP_PID=$!
trap 'kill $HTTP_PID 2>/dev/null' EXIT
WAS_RUNNING=0
[ -f "$ROOT/domain-server.pid" ] && kill -0 "$(cat "$ROOT/domain-server.pid")" 2>/dev/null && WAS_RUNNING=1
stop_sandbox
settings on
SANDBOX_ASSIGNMENTS=7 "$HOME/git/tivolicloud/domain-sandbox.sh" wasm "$BUILD" 43102 > /dev/null
LOG="$ROOT/logs/assignment-client.log"
for i in $(seq 1 90); do
    sleep 1
    # printed lines only: the log also quotes the script's source as it is downloaded
    grep "AVATAR-SOUND RESULT" "$LOG" 2>/dev/null | grep -qv '\\n' && break
done
grep -h "AVATAR-SOUND" "$LOG" | grep -v '\\n' | sed 's/.*AVATAR-SOUND /AVATAR-SOUND /' > "$OUT/lines.txt" || true
cp "$LOG" "$OUT/assignment-client.log" 2>/dev/null || true
stop_sandbox
settings off
if [ $WAS_RUNNING = 1 ]; then
    "$HOME/git/tivolicloud/domain-sandbox.sh" wasm "$BUILD" 43102 > /dev/null
fi
echo "== the lines:"
cat "$OUT/lines.txt"
grep -qE "RESULT (.* )?PASS" "$OUT/lines.txt"
