# harness.sh: sourced by the physics-baseline runners (PHYSICS.md round 9), and the shared test launcher for any test
# that starts clients on a shared machine. The pieces every runner needs, written once: clients walled off from the
# user's own settings and identity, small in a corner, titled as tests and taking turns through the machine lock, the
# sandbox started and stopped by the pids it recorded, the earlier runs' entities taken out of its world, and each
# client's ktx cache deleted when it exits (0.3 to 1.5 GB each; forty of them filled the disk on 10-18). POSIX sh, so
# the sh runners (run.sh, run-pair.sh) and the zsh ones can all source it. The caller sets BIN (the client), R (the
# results dir), and if it likes SESSION (who runs the test: in the title and the lock), CLIENT_WINDOW, SANDBOX,
# DOMAIN_SANDBOX and MACHINE_LOCK before sourcing.
#
#   wall <name>                      $R/<name>-home with config, data and snaps, Interface.json seeded with the
#                                    snapshots folder (without it a folder dialog blocks the client) and the window's
#                                    place, CLIENT_WINDOW: 800 x 600 (the least the client takes) at the bottom right
#                                    of a 1920 x 1080 screen unless the caller says otherwise
#   client <name> <args...>          the client walled off in $R/<name>-home, --allowMultipleInstances (a second
#                                    client would otherwise hand its --url to one already running and exit) and
#                                    --tokens "{}" (no sign-in), stdout and stderr to $R/<name>.log; the ktx cache
#                                    is deleted when it exits (kept with KEEP_KTX=1, for a warm run of the same
#                                    client next). Titled "Darkride - $SESSION - TEST, not yours" ("QUIET TEST" while
#                                    the lock is held --quiet) unless the runner passes its own --clientLabel; the
#                                    client takes its Dock icon from those words (TestClientIcon.h). Takes the machine lock first (lock_take), unless
#                                    this run holds it already. Environment set by the caller (TIVOLI_PACKET_LOSS,
#                                    TIVOLI_PACKET_DELAY_MS) passes through. On macOS it starts in the background,
#                                    never in front of the window the user is typing in (see client_start).
#   client_start <log> <args...>     starts $BIN with its output in <log> and waits for it to quit. On macOS, through
#                                    `open -g -n -W` with Qt's activation switched off: a binary run straight from a
#                                    shell is brought to the front by macOS at launch and by Qt when it has launched,
#                                    and takes the keyboard. open passes on only the environment it's given, so the
#                                    TIVOLI_, HIFI_, DARKRIDE_, QT_, XDG_, DYLD_ and CFFIXED_ variables are handed
#                                    over; the client's exit status isn't, so once it has gone the system log's
#                                    record of its end is read instead (client_exit_line).
#                                    A release build (bundle id com.tivolicloud.interface: 1.0.x, PRODUCTION builds)
#                                    shares the installed Darkride's "Tivoli Cloud VR" folders, and 1.0.x writes its
#                                    log, QML cache and web data there whatever XDG_CONFIG_HOME and TIVOLI_APP_DATA_DIR
#                                    say: such a client gets CFFIXED_USER_HOME=<log>.home (where Foundation, and so Qt,
#                                    puts ~/Library; HOME doesn't move it) unless the caller set one, and once it has
#                                    quit, "WALLED OFF" or "NOT WALLED OFF" says whether ~/Library's Tivoli Cloud VR
#                                    folders changed while it ran. Its caches, the ktx cache too (unless KEEP_KTX=1),
#                                    are deleted after; its log stays. CLIENT_FOREGROUND=1 opens it in front
#                                    instead, still through open (a client someone plays, or a timing run that needs
#                                    the window on top). Elsewhere than macOS it runs the binary straight.
#                                    The client's pid goes in <log>.pid while it runs; CLIENT_TIMEOUT=<s> stops a
#                                    client still running after that long (SIGTERM, then SIGKILL after 5 s).
#                                    CLIENT_OPEN_URL=<url> (macOS) gives the starting client a link the way a browser
#                                    does (the get-URL Apple Event), to that bundle only; --url takes another path.
#                                    client_start also passes -NSAutoFillHeuristicsEnabled NO (AppKit's argument domain;
#                                    the client's own parser lists it as unknown and goes on): AutoFill starts a
#                                    SafariPlatformSupport.Helper when the app becomes active, and the helper outlives
#                                    the client; 422 of them used up launchservicesd and brought a Mac down
#                                    (2026-10-25). The background launch never activates it; this covers a click.
#                                    It passes --noAudioInput too (no microphone, so no macOS permission prompt over
#                                    the test); CLIENT_MIC=1 keeps the microphone for a test that needs it.
#                                    When the client has quit, the macOS helper services it left running are stopped
#                                    (stop_client_helpers), whichever way it was launched.
#   stop_client_helpers <pid> <since>
#                                    stops the VT decoder and encoder services and the Safari helper whose own logs
#                                    name client <pid> since <since> ("YYYY-mm-dd HH:MM:SS"), and nothing else
#   client_exit_line <pid> <log> [<since>]
#                                    one line when client <pid> has gone (macOS): when, how it ended (its exit status,
#                                    or the signal that ended it, from launchd's record in the system log since <since>,
#                                    "YYYY-mm-dd HH:MM:SS", or the last 2 minutes) and when its log last wrote; printed,
#                                    and added to the end of <log>
#   lock_take [purpose] [minutes] [--quiet]   (LOCK_FLAGS=--quiet or --front in the environment asks the same for every
#                                             take of the run, client_start's included)
#                                    waits for the machine lock and holds it until this runner exits (or lock_give):
#                                    one test with windows on the GPU at a time. --quiet for timing runs (no builds).
#                                    Without the lock script (another machine) it says so and goes on.
#   lock_give                        lets the machine lock go before the runner ends
#   sandbox_stop                     stops this sandbox's servers by the pids in $SANDBOX/*.pid, each checked to be a
#                                    domain-server or assignment-client of ours, and deletes the pid files
#   sandbox_clean_world <name>...    takes the entities whose name starts with (or is) one of the names out of the
#                                    sandbox's world file; an earlier run's entities (lifetime 600 s) would otherwise
#                                    still be in the world beside this run's
#   sandbox_start <build> <port>     turns the physics server on in the sandbox's config (off by default), starts the
#                                    sandbox from the build with domain-sandbox.sh and waits for the physics server
#                                    (the sandbox's monitor adds the seventh service at its spare check, up to 30 s)
#   sandbox_copy_logs                copies the sandbox's assignment-client and domain-server logs beside the results
#                                    (a restart overwrites them)
#   wait_for_client <name> [s]       waits until the client's log says its physics is on (its loading screen has lifted:
#                                    the world is in), up to s seconds (180): the second client of a pair must not start
#                                    before the first has the world, or it misses the cabin (a cold cache under 5 % loss
#                                    took 90 s to load the sandbox's world on 10-19, 20 s without loss). The time a
#                                    client waits for the machine lock before it starts doesn't count.

SANDBOX=${SANDBOX:-$HOME/tivoli-sandboxes/physics}
DOMAIN_SANDBOX=${DOMAIN_SANDBOX:-$HOME/git/tivolicloud/domain-sandbox.sh}   # darkride-ops' sandbox starter
MACHINE_LOCK=${MACHINE_LOCK:-$HOME/git/tivolicloud/machine-lock.sh}         # darkride-ops' lock for the shared Mac
# who runs the test, in the window title and the machine lock; physics' runners came first and keep the default
[ -n "${SESSION:-}" ] || echo "harness.sh: SESSION not set, so titles and the machine lock say \"physics\" (set SESSION=<your session>)" >&2
SESSION=${SESSION:-physics}
CLIENT_WINDOW=${CLIENT_WINDOW:-@Rect(1110 470 800 600)}
export HIFI_METAVERSE_URL=${HIFI_METAVERSE_URL:-https://vr.darkride.ai}

wall() {
    mkdir -p "$R/$1-home/data" "$R/$1-home/snaps"
    # a dev build keeps its settings under "Tivoli Cloud VR - dev", a PRODUCTION build under "Tivoli Cloud VR" (avatars)
    for _org in "Tivoli Cloud VR - dev" "Tivoli Cloud VR"; do
        mkdir -p "$R/$1-home/config/$_org"
        [ -f "$R/$1-home/config/$_org/Interface.json" ] ||
            echo "{\"snapshotsLocation\": \"$R/$1-home/snaps\", \"WindowGeometry\": \"$CLIENT_WINDOW\"}" \
                > "$R/$1-home/config/$_org/Interface.json"
    done
}

# --pid $$: the lock is this runner's ($$ is the runner's shell in a background client too) and is freed when it exits;
# LOCK_PID=<pid> ties it to another process instead (a launcher that returns while its client runs, as social's does)
lock_take() {
    if [ ! -x "$MACHINE_LOCK" ]; then
        [ -n "${_lock_said:-}" ] || echo "no machine lock at $MACHINE_LOCK: going on without one"
        _lock_said=1; return 0
    fi
    # LOCK_FLAGS=--quiet (or --front) in the environment asks for a quiet machine: builds through machine-lock.sh build
    # wait, and the take waits for running ones; empty = a plain take, as before (physics, 2026-11-04)
    "$MACHINE_LOCK" take "$SESSION" "${1:-test run $(basename "$R")}" "${2:-30}" --pid "${LOCK_PID:-$$}" ${3:+"$3"} ${LOCK_FLAGS:-}
}

lock_give() {
    [ ! -x "$MACHINE_LOCK" ] || "$MACHINE_LOCK" give "$SESSION"
}

# stop_client_helpers <client pid> <since>: stops the macOS helper services a client left running when it quit
# (performance, 2026-10-25): VTDecoderXPCService and VTEncoderXPCService (~31 MB a pair, from QtWebEngine checking the
# video formats at start-up, whichever way the client was launched), com.apple.SafariPlatformSupport.Helper, and
# MTLCompilerService (13-54 MB, left by a client that compiled shaders; performance, 10-25).
# Each logs "invalidated because the client process (pid N) ... exited" and stays alive. Only the helpers whose own log
# names this client are stopped, by pid, after checking the pid is still that service: never another app's.
stop_client_helpers() {
    _helpers=$(/usr/bin/log show --start "$2" --style compact --predicate \
        "(process == \"VTDecoderXPCService\" OR process == \"VTEncoderXPCService\" OR process == \"com.apple.SafariPlatformSupport.Helper\" OR process == \"MTLCompilerService\") AND eventMessage CONTAINS \"client process (pid $1)\"" \
        2>/dev/null |
        grep -oE '(VTDecoderXPCService|VTEncoderXPCService|com\.apple\.SafariPlatformSupport\.Helper|MTLCompilerService)\[[0-9]+' | sort -u)
    [ -n "$_helpers" ] || return 0
    # SIGTERM, then after 3 s SIGKILL for any that's still alive and still that service (social's fallback)
    for _signal in TERM KILL; do
        [ "$_signal" = TERM ] || sleep 3
        for _helper in $(printf '%s\n' "$_helpers"); do
            _service=${_helper%%\[*}; _hpid=${_helper##*\[}
            ps -o command= -p "$_hpid" 2>/dev/null | grep -q "$_service" || continue
            # a service can serve several processes (an MTLCompilerService serves WindowServer too): kept while any
            # peer of its own, other than this client, is still alive (performance's check, 10-25)
            if [ "$_signal" = TERM ]; then
                _started=$(date -j -f "%a %b %d %T %Y" "$(ps -o lstart= -p "$_hpid")" '+%Y-%m-%d %H:%M:%S' 2>/dev/null)
                _others=$(/usr/bin/log show --start "${_started:-$2}" --style compact \
                    --predicate "processID == $_hpid AND eventMessage CONTAINS \"peer[\"" 2>/dev/null |
                    grep -oE 'peer\[[0-9]+' | grep -oE '[0-9]+' | sort -u | grep -vx "$1")
                _busy=""
                for _peer in $(printf '%s\n' "$_others"); do
                    # ps, not kill -0: a peer can be another user's (WindowServer), which kill -0 can't signal
                    ps -p "$_peer" > /dev/null 2>&1 && _busy="$_busy $_peer"
                done
                if [ -n "$_busy" ]; then
                    echo "kept $_service $_hpid: it also serves$_busy"
                    continue
                fi
            fi
            kill -"$_signal" "$_hpid" && echo "stopped $_service $_hpid ($_signal), left by client $1"
        done
    done
}

# How a client launched through open ended (performance, qa, 10-28): open's own status says nothing about the client,
# which isn't the runner's child. launchd reports each app's end to RunningBoard, which logs it as
# "[app<...>:<pid>] termination reported by launchd (<namespace>, <code>, <wait status>)": namespace 0 is the app's own
# exit (the status as wait gives it), 2 a signal (code = its number), 15 RunningBoard's own kill. Two clients that
# vanished with no crash report on 10-28 were SIGSEGV there.
client_exit_line() {
    _gone=$(date '+%H:%M:%S')
    _entry=""; _try=0
    while [ -z "$_entry" ] && [ "$_try" -lt 5 ]; do
        [ "$_try" = 0 ] || sleep 1
        _entry=$(/usr/bin/log show --style compact --start "${3:-$(date -v-2M '+%Y-%m-%d %H:%M:%S')}" --predicate \
            "process == \"runningboardd\" AND eventMessage CONTAINS \":$1] termination reported by launchd\"" 2>/dev/null |
            grep -E 'termination reported by launchd \([0-9-]+, [0-9-]+, [0-9-]+\)' | tail -1)
        _try=$((_try + 1))
    done
    # the record's own time (compact style: "YYYY-mm-dd HH:MM:SS.mmm ..."), and its three numbers
    [ -z "$_entry" ] || _gone=$(printf '%s\n' "$_entry" | cut -c12-23)
    _record=$(printf '%s\n' "$_entry" | grep -oE 'launchd \([0-9-]+, [0-9-]+, [0-9-]+\)' | grep -oE '[0-9-]+, [0-9-]+, [0-9-]+')
    if [ -z "$_record" ]; then
        _how="no record of its end in the system log"
    else
        _ns=${_record%%,*}; _rest=${_record#*, }; _code=${_rest%%,*}; _wait=${_rest#*, }
        _sig=$((_wait & 127))
        if [ "$_ns" = 2 ]; then
            _sig=$_code
        fi
        if [ "$_ns" = 0 ] && [ "$_sig" = 0 ]; then
            _how="exited with status $((_wait >> 8))"
        elif [ "$_ns" = 0 ] || [ "$_ns" = 2 ]; then
            _how="ended by signal SIG$(kill -l "$_sig" 2>/dev/null || echo "$_sig")"
            case "$_sig" in
                4|5|6|7|8|10|11) _how="$_how, a crash" ;;
            esac
        elif [ "$_ns" = 15 ]; then
            _how="stopped by macOS (RunningBoard), wait status $_wait"
        else
            _how="ended (launchd's record: $_record)"
        fi
    fi
    _last=$(tail -n 400 "$2" 2>/dev/null | grep -aoE '^\[[0-9/]+ [0-9:]+\]' | tail -1)
    _line="client_start: client $1 gone at $_gone, $_how; its log's last line at ${_last:-(none)}"
    echo "$_line" >> "$2"
    case "$_how" in
        "exited with status 0") echo "$_line" ;;
        *) echo "$_line" >&2 ;;
    esac
}

client_start() {
    _log=$1; shift
    case "$BIN" in
        */Contents/MacOS/*) _app=${BIN%/Contents/MacOS/*} ;;
        *) _app="" ;;
    esac
    # only a Darkride.app: build trees can still hold a pre-rename interface.app whose binary no longer matches the
    # tree's libraries, and it crashes at load with a dialog in front (10-25 13:49); CLIENT_ANY_APP=1 to allow another
    case "$_app" in
        */Darkride.app|"") ;;
        *) [ -n "${CLIENT_ANY_APP:-}" ] || { echo "client_start: $_app isn't a Darkride.app (a stale build?); CLIENT_ANY_APP=1 to start it anyway" >&2; return 1; } ;;
    esac
    # open takes only absolute paths: a relative app is "Unable to find application", and nothing starts (physics)
    if [ -n "$_app" ]; then
        _app=$(cd "$_app" 2>/dev/null && pwd) || { echo "client_start: no app at $BIN" >&2; return 1; }
    fi
    case "$_log" in /*) ;; *) _log="$(pwd)/$_log" ;; esac
    # --noAudioInput: the client opens no microphone, so a rebuilt (ad-hoc signed) build raises no macOS microphone
    # prompt in front of the test; CLIENT_MIC=1 for a test that needs the microphone. Older builds list the option as
    # unknown and go on
    [ -n "${CLIENT_MIC:-}" ] || set -- --noAudioInput "$@"
    _since=$(date '+%Y-%m-%d %H:%M:%S')
    # a release build's own home, away from the user's ~/Library (see the header); the user's folders are checked after
    _wallhome=""
    if [ -n "$_app" ] && [ -z "${CFFIXED_USER_HOME:-}" ] &&
       [ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$_app/Contents/Info.plist" 2>/dev/null)" = com.tivolicloud.interface ]; then
        _wallhome="$_log.home"
        mkdir -p "$_wallhome"
    fi
    [ -z "$_wallhome${CFFIXED_USER_HOME:-}" ] || : > "$_log.started"
    if [ -z "$_app" ] || [ "$(uname)" != Darwin ]; then
        "$BIN" "$@" > "$_log" 2>&1 &
        _cpid=$!
        wait $_cpid
        _status=$?
        [ "$(uname)" != Darwin ] || stop_client_helpers "$_cpid" "$_since"
        return $_status
    fi
    # -DarkrideTestRun <log> marks this client's command line (an AppKit default nothing reads), so its pid can be
    # found while it runs: open's own pid is $!, and pgrep for the binary alone could find someone else's client
    # --allowMultipleInstances always: without it a client hands its arguments to a Darkride already running and quits
    set -- --args -DarkrideTestRun "$_log" -NSAutoFillHeuristicsEnabled NO --allowMultipleInstances "$@"
    # CLIENT_OPEN_URL=<url>: open hands the URL to this bundle as a link clicked in a browser does (the get-URL Apple
    # Event, a QFileOpenEvent in the client) while it starts, whichever app the scheme's links go to and whether or not
    # the bundle claims the scheme
    [ -z "${CLIENT_OPEN_URL:-}" ] || set -- "$CLIENT_OPEN_URL" "$@"
    set -- -a "$_app" "$@"
    for _var in $(env | grep -E '^(TIVOLI|HIFI|DARKRIDE|QT|XDG|DYLD|CFFIXED)_[A-Za-z0-9_]*=' | cut -d= -f1); do
        eval "_value=\${$_var}"
        set -- --env "$_var=$_value" "$@"
    done
    [ -z "$_wallhome" ] || set -- --env "CFFIXED_USER_HOME=$_wallhome" "$@"
    : > "$_log"
    if [ -n "${CLIENT_FOREGROUND:-}" ]; then
        # in front, for a client someone plays: macOS and Qt activate it as a normal launch would
        open -n -W --stdout "$_log" --stderr "$_log" "$@" &
    else
        QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM=1 QT_MAC_SET_RAISE_PROCESS=0 \
            open -g -n -W --env QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM=1 --env QT_MAC_SET_RAISE_PROCESS=0 \
                --stdout "$_log" --stderr "$_log" "$@" &
    fi
    _opener=$!
    _cpid=""; _i=0
    while [ -z "$_cpid" ] && [ "$_i" -lt 120 ] && kill -0 $_opener 2>/dev/null; do
        _cpid=$(pgrep -f -- "-DarkrideTestRun $_log" | head -1)
        _i=$((_i + 1)); sleep 0.5
    done
    # the client's pid beside its log, and CLIENT_TIMEOUT=<s>: SIGTERM after that long, SIGKILL 5 s later (the client
    # has been seen to ignore SIGTERM); open -W alone would wait for ever (scripting's run-client.sh does the same).
    # The watcher's sleeps write nowhere and are stopped with it: a sleep left behind holding the runner's stdout
    # made a runner piped into tail wait up to CLIENT_TIMEOUT after the client quit
    _watch=""
    if [ -n "$_cpid" ]; then
        echo "$_cpid" > "$_log.pid"
        if [ -n "${CLIENT_TIMEOUT:-}" ]; then
            ( _nap=""; trap '[ -z "$_nap" ] || kill "$_nap" 2>/dev/null; exit 0' TERM
              sleep "$CLIENT_TIMEOUT" >/dev/null 2>&1 & _nap=$!; wait "$_nap"; _nap=""
              kill -0 "$_cpid" 2>/dev/null || exit 0
              echo "client_start: client $_cpid still running after ${CLIENT_TIMEOUT}s: stopping it" >&2
              kill -TERM "$_cpid"; sleep 5 >/dev/null 2>&1 & _nap=$!; wait "$_nap"; _nap=""
              kill -0 "$_cpid" 2>/dev/null && kill -KILL "$_cpid" ) &
            _watch=$!
        fi
    fi
    wait $_opener
    _status=$?
    [ -z "$_watch" ] || kill "$_watch" 2>/dev/null
    [ -z "$_cpid" ] || client_exit_line "$_cpid" "$_log" "$_since"
    [ -z "$_cpid" ] || stop_client_helpers "$_cpid" "$_since"
    if [ -z "$_cpid" ]; then
        echo "client_start: no client started from $_app (open's status $_status); see above and $_log" >&2
        [ "$_status" != 0 ] || _status=1
    fi
    if [ -n "$_wallhome" ] || [ -n "${CFFIXED_USER_HOME:-}" ]; then
        _touched=$(find "$HOME/Library/Application Support/Tivoli Cloud VR" "$HOME/Library/Caches/Tivoli Cloud VR" \
                       -newer "$_log.started" -type f 2>/dev/null | wc -l | tr -d ' ')
        if [ "$_touched" = 0 ]; then
            echo "client_start: WALLED OFF: nothing in ~/Library's Tivoli Cloud VR folders changed"
        else
            echo "client_start: NOT WALLED OFF: $_touched files in ~/Library's Tivoli Cloud VR folders changed while" \
                 "client ${_cpid:-?} ran (or the user's own Darkride wrote them): find them with -newermt \"$_since\"" >&2
        fi
        rm -f "$_log.started"
        # its caches (QML, web, and the ktx cache, 0.3 to 1.5 GB, which 1.0.x keeps beside its log in Application
        # Support; KEEP_KTX=1 keeps it): the log stays, in $_wallhome/Library/Application Support
        if [ -n "$_wallhome" ]; then
            rm -rf "$_wallhome/Library/Caches"
            [ -n "${KEEP_KTX:-}" ] || find "$_wallhome" -type d -name ktx_cache -prune -exec rm -rf {} +
        fi
    fi
    return $_status
}

client() {
    _name=$1; shift
    wall "$_name"
    lock_take || return 1
    # QUIET in the title (and the Dock icon) while this run holds the machine lock for a quiet timing run
    _test="TEST"
    [ ! -x "$MACHINE_LOCK" ] || ! "$MACHINE_LOCK" quiet || _test="QUIET TEST"
    DARKRIDE_CLIENT_LABEL=${DARKRIDE_CLIENT_LABEL:-"Darkride - $SESSION - $_test, not yours"} \
    XDG_CONFIG_HOME="$R/$_name-home/config" TIVOLI_APP_DATA_DIR="$R/$_name-home/data" \
        client_start "$R/$_name.log" --allowMultipleInstances --tokens "{}" "$@"
    _status=$?
    [ -n "${KEEP_KTX:-}" ] || rm -rf "$R/$_name-home/data/ktx_cache"
    return $_status
}

sandbox_stop() {
    for _pidfile in $(find "$SANDBOX" -maxdepth 1 -name '*.pid' 2>/dev/null); do
        _pid=$(cat "$_pidfile" 2>/dev/null)
        if [ -n "$_pid" ] && ps -o command= -p "$_pid" 2>/dev/null | grep -q "domain-server\|assignment-client"; then
            kill "$_pid" 2>/dev/null || true
        fi
        rm -f "$_pidfile"
    done
    sleep 3
}

sandbox_clean_world() {
    # both the entity server's file and the domain-server's copy (appdata/entities), which replaces the entity
    # server's at start when its data version is newer (see run-loading.sh)
    python3 - "$SANDBOX/entities/models.json.gz" "$SANDBOX/appdata/entities/models.json.gz" "$@" <<'PY'
import gzip, json, os, sys
paths, names = sys.argv[1:3], sys.argv[3:]
for path in paths:
    if not os.path.exists(path):
        continue
    try:
        world = json.load(gzip.open(path))
    except Exception as e:
        print("no world file to clean at", path, ":", e); continue
    before = len(world.get("Entities", []))
    world["Entities"] = [e for e in world.get("Entities", [])
                         if not any(str(e.get("name", "")).startswith(n) for n in names)]
    with gzip.open(path, "wt") as f:
        json.dump(world, f)
    print("world cleaned of earlier runs' test entities:", before - len(world["Entities"]), "removed,", len(world["Entities"]), "kept, in", path)
PY
}

sandbox_start() {
    _build=$1; _port=$2
    python3 -c 'import json, os, sys; p = sys.argv[1]; c = json.load(open(p)) if os.path.exists(p) else {}; c.setdefault("physics_server", {})["enabled"] = True; json.dump(c, open(p, "w"), indent=2)' "$SANDBOX/domain-config.json"
    # the sandbox's name is its folder's (domain-sandbox.sh keeps ~/tivoli-sandboxes/<name>), so sandbox_stop and
    # sandbox_clean_world, which use $SANDBOX, act on the same one (skunkworks, 10-25: it was always "physics")
    "$DOMAIN_SANDBOX" "$(basename "$SANDBOX")" "$_build" "$_port"
    _i=0
    while [ "$_i" -lt 90 ]; do
        _i=$((_i + 1))
        curl -s "http://localhost:$((_port - 2))/nodes.json" 2>/dev/null | grep -q '"physics-server"' && break
        sleep 1
    done
    echo "physics-server in the domain after ${_i}s"
}

wait_for_client() {
    _name=$1; _limit=${2:-180}; _i=0
    # the log appears when the client starts, after its turn at the machine lock: the wait for that doesn't count
    _w=0
    while [ ! -f "$R/$_name.log" ] && [ "$_w" -lt 7200 ]; do _w=$((_w + 1)); sleep 1; done
    while [ "$_i" -lt "$_limit" ]; do
        grep -q "Physics enabled" "$R/$_name.log" 2>/dev/null && break
        _i=$((_i + 1)); sleep 1
    done
    echo "$_name has the world after ${_i}s"
}

sandbox_copy_logs() {
    cp "$SANDBOX/logs/assignment-client.log" "$R/sandbox-assignment-client.log" 2>/dev/null || true
    cp "$SANDBOX/logs/domain-server.log" "$R/sandbox-domain-server.log" 2>/dev/null || true
}
