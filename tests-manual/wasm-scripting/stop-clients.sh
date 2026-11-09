#!/bin/sh
# Stops every client run-client.sh started (a client process whose log, named by its -DarkrideTestRun argument, lies
# under this folder's results/), and nothing else: another session's client, even one running this checkout's build
# (qa's, 2026-10-30), is not ours to stop. SIGTERM first, SIGKILL after 5 s; then the macOS helper services each one
# left (release's stop_client_helpers in tests-manual/physics-baseline/harness.sh, bug #29). Run it before and after
# every test run.
#   tests-manual/wasm-scripting/stop-clients.sh
D=$(cd "$(dirname "$0")" && pwd)
RESULTS="$D/results/"
SESSION=${SESSION:-scripting}
. "$D/../physics-baseline/harness.sh"
my_clients() {
    ps -ax -o pid=,command= | grep -F -- "-DarkrideTestRun $RESULTS" | grep -v grep | awk '{print $1}'
}
PIDS=$(my_clients)
if [ -z "$PIDS" ]; then
    echo "no clients of $RESULTS are running"
    exit 0
fi
STARTED=""
for P in $PIDS; do
    # its start, for the helpers' logs ("YYYY-mm-dd HH:MM:SS", kept here as YYYY-mm-dd_HH:MM:SS)
    SINCE=$(date -j -f '%a %b %d %T %Y' "$(LC_ALL=C ps -o lstart= -p $P)" '+%Y-%m-%d_%H:%M:%S' 2>/dev/null)
    STARTED="$STARTED $P=${SINCE:-$(date '+%Y-%m-%d')_00:00:00}"
    echo "stopping $P: $(ps -o etime= -p $P | tr -d ' ') old, $(ps -o command= -p $P | cut -c1-80)"
    kill $P 2>/dev/null
done
sleep 5
for P in $PIDS; do
    if kill -0 $P 2>/dev/null; then
        echo "$P did not stop, killing it"
        kill -9 $P 2>/dev/null
    fi
done
sleep 1
for ENTRY in $STARTED; do
    SINCE=${ENTRY#*=}
    stop_client_helpers "${ENTRY%%=*}" "$(echo "$SINCE" | tr _ ' ')"
done
echo "remaining: $(my_clients | wc -l | tr -d ' ')"
