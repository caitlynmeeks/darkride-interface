#!/bin/sh
# The one way a test client of this checkout is started: with the absolute path of the build's client, its own
# settings and data under results/<name>, a hard time limit, the machine lock, and every client of this checkout
# stopped before and after. Prints the client's pid; the log is results/<name>/client.log.
#   tests-manual/wasm-scripting/run-client.sh <name> <seconds> [client arguments...]
# The client starts through release's client_start (tests-manual/physics-baseline/harness.sh): `open -g` in the
# background with AppKit's AutoFill off, and the macOS helper services it leaves are stopped when it quits (bug #29: 422
# leftover helpers brought the Mac down on 2026-10-25). Never start a client straight from a shell.
# Environment: DARKRIDE_RUN_JS_ON_WASM=1 or 0 (or DARKRIDE_RUN_JS_ON_QTSCRIPT=1) picks the engine for this client;
# HIFI_METAVERSE_URL names the metaverse server; DARKRIDE_CLIENT_LABEL is the label the window title starts with;
# CLIENT_APP is another Darkride.app to start instead of this checkout's (run.sh, run-wasm.sh); CLIENT_RESULTS is a
# results folder of the caller's instead of results/<name> (kept, not emptied).
# A scripted test client (given --testScript) has the shape of the team rules' standing permission: a small window in
# the bottom-right corner, behind the others, its own config folder, signed out, no metaverse (a dead address unless
# given), titled "Darkride - scripting - TEST, not yours"; quitWhenFinished follows its script. A client for a
# person's hands keeps the sign-in screen and https://vr.darkride.ai, opens in front, and is titled
# "Darkride - scripting - <name>".
set -e
D=$(cd "$(dirname "$0")" && pwd)
SCRIPTED=0
for ARG in "$@"; do
    [ "$ARG" = "--testScript" ] && SCRIPTED=1
done
if [ $SCRIPTED = 1 ]; then
    export HIFI_METAVERSE_URL=${HIFI_METAVERSE_URL:-http://127.0.0.1:9}
else
    # the metaverse server a person signs in to and finds worlds on; the client's built-in default is out of date
    export HIFI_METAVERSE_URL=${HIFI_METAVERSE_URL:-https://vr.darkride.ai}
fi
NAME=$1
SECONDS_MAX=$2
shift 2
[ -n "$NAME" ] && [ -n "$SECONDS_MAX" ] || { echo "usage: $0 <name> <seconds> [client arguments...]" >&2; exit 1; }
if [ $SCRIPTED = 1 ]; then
    export DARKRIDE_CLIENT_LABEL=${DARKRIDE_CLIENT_LABEL:-"Darkride - scripting - TEST, not yours"}
else
    export DARKRIDE_CLIENT_LABEL=${DARKRIDE_CLIENT_LABEL:-"Darkride - scripting - $NAME"}
    CLIENT_FOREGROUND=1
fi
APP=${CLIENT_APP:-"$D/../../build/interface/Darkride.app"}
BIN="$APP/Contents/MacOS/Darkride"
BIN=$(cd "$(dirname "$BIN")" && pwd)/Darkride
[ -x "$BIN" ] || { echo "no client at $BIN" >&2; exit 1; }
# A scripted client (given --testScript) goes straight in, signed out: the sign-in screen would keep its script from
# running. `--tokens` and `{}` as two arguments; safe only because this client's settings and account file are its
# own. quitWhenFinished goes straight after the script's path (the client reads it only there).
COUNT=$#
AFTER_SCRIPT=0
while [ $COUNT -gt 0 ]; do
    ARG=$1
    shift
    COUNT=$((COUNT - 1))
    set -- "$@" "$ARG"
    if [ $AFTER_SCRIPT = 1 ]; then
        AFTER_SCRIPT=0
        if [ $COUNT = 0 ] || [ "$1" != quitWhenFinished ]; then
            set -- "$@" quitWhenFinished
        fi
    fi
    [ "$ARG" = "--testScript" ] && AFTER_SCRIPT=1
done
if [ $SCRIPTED = 1 ]; then
    set -- "$@" --tokens "{}"
fi
# the label on the command line as well as in DARKRIDE_CLIENT_LABEL, so a look at the process list finds it
set -- "$@" --clientLabel "$DARKRIDE_CLIENT_LABEL"
if [ -n "$CLIENT_RESULTS" ]; then
    R=$CLIENT_RESULTS
else
    R="$D/results/$NAME"
    rm -rf "$R"
fi
# a dev build keeps its settings under "Tivoli Cloud VR - dev", a production package under "Tivoli Cloud VR"
mkdir -p "$R/config/Tivoli Cloud VR - dev" "$R/config/Tivoli Cloud VR" "$R/data"
if [ $SCRIPTED = 1 ]; then
    # small, in the bottom-right corner. No snapshotsLocation here: with one set, a client on QtScript that quits
    # within seconds of starting crashes in tivoli.js's start-up evaluation (KNOWN-ISSUES, 2026-10-25)
    for _org in "Tivoli Cloud VR - dev" "Tivoli Cloud VR"; do
        echo "{\"WindowGeometry\": \"@Rect(1110 470 800 600)\"}" > "$R/config/$_org/Interface.json"
    done
fi
SESSION=scripting
SANDBOX=$HOME/tivoli-sandboxes/wasm
. "$D/../physics-baseline/harness.sh"
lock_take "run-client $NAME" $((SECONDS_MAX / 60 + 2)) || exit 1
# a client of ours left from an earlier run goes now, under the lock (stop-clients.sh touches only our own)
"$D/stop-clients.sh" > /dev/null
LOG="$R/client.log"
XDG_CONFIG_HOME="$R/config" TIVOLI_APP_DATA_DIR="$R/data" CLIENT_FOREGROUND=$CLIENT_FOREGROUND \
    client_start "$LOG" --allowMultipleInstances "$@" &
STARTER=$!
# client_start knows its client by the -DarkrideTestRun <log> it passes; so does the time limit here
PID=""
WAITED=0
while [ -z "$PID" ] && [ $WAITED -lt 120 ] && kill -0 $STARTER 2>/dev/null; do
    PID=$(pgrep -f -- "-DarkrideTestRun $LOG" | head -1 || true)
    [ -n "$PID" ] || sleep 0.5
    WAITED=$((WAITED + 1))
done
if [ -z "$PID" ]; then
    wait $STARTER || true
    echo "no client started; see $LOG" >&2
    exit 1
fi
echo "$PID" > "$R/pid"
echo "client $PID, log $LOG"
WAITED=0
while kill -0 $PID 2>/dev/null; do
    if [ $WAITED -ge "$SECONDS_MAX" ]; then
        echo "time limit of $SECONDS_MAX s reached, stopping the client"
        kill $PID 2>/dev/null || true
        sleep 5
        kill -0 $PID 2>/dev/null && kill -9 $PID 2>/dev/null
        break
    fi
    sleep 1
    WAITED=$((WAITED + 1))
done
# client_start returns once the client has quit, after stopping the helper services it left
wait $STARTER || true
"$D/stop-clients.sh" > /dev/null
rm -f "$R/pid"
rm -rf "$R/data/ktx_cache"
lock_give
