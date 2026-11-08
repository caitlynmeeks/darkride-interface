#!/usr/bin/env python3
#
#  channel-kit.py
#  tests-manual/make-link
#
#  Make's channel to Claude Code, from the Claude Code side, with no Claude and nothing spent: a scripted MCP client
#  speaks to a running test client's Make through the darkride-mcp helper, as a Claude Code started with Make's chat
#  line would. Prints one line per check or event and "CHANNEL KIT PASS" or "CHANNEL KIT FAIL n".
#    python3 -I channel-kit.py <the test client's Make folder> [--mode check|chat|silent] [--line chat|plain|edited|none]
#                              [--version 2.1.319] [--seconds 10] [--log <the test client's log>]
#  --mode check  (default) what a channel client must see: the initialize reply (the protocol held at 2025-11-25 when
#                2026-07-28 is asked, "claude/channel" the only experimental capability, never the permission relay),
#                the reply tool taking msg_id and the picture tool taking reference, a reply by msg_id shown, a
#                reference nobody attached refused as not_found, and no channel event arriving unprompted for
#                --seconds (nothing but the dock's Send makes one).
#  --mode chat   a stand-in for the Claude Code in Terminal, for editor-ui's dock kit: for --seconds it takes each
#                channel event the dock's Send makes, answers it with reply {msg_id, text}, and fetches the first
#                attached picture by reference; one line per event.
#  --mode silent the same, but it never calls a tool (the dock's "Claude didn't get your message" line after 30 s).
#  --line chat   the kit re-runs itself with exactly Make's chat line as its arguments (its own settings kept in the
#                environment), so the app, reading the helper's parent, sees channelFlag "on"; --line plain gives
#                the plain line ("off"); none (default) leaves the kit's own arguments ("unknown": allowed).
#  --line edited Make's chat line plus one option its line check refuses (`--add-dir /tmp`, or the arguments in
#                CHANNEL_KIT_EXTRA): the app drops the helper as it connects and the dock shows the refused words
#                (C9, editor-ui's kit 6); the kit expects no answer to initialize and, with --log <the test client's
#                log>, Make's own refusal line written in this run; PASS needs both.
#  --version     what the kit says as clientInfo.version (an old one proves the dock's "Update Claude Code" line).
#  Kits 1 and 2 of the plan (the dock's Send makes one event; nothing else does) are editor-ui's dock kit, which
#  reads MakeLink::channelEventCount(); this kit is the other end of them.
#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#

import json, os, select, shlex, subprocess, sys, time

CHANNEL_FLAG = "--dangerously-load-development-channels"
CHANNEL_ENTRY = "server:darkride"


def option(name, default):
    if name in sys.argv:
        return sys.argv[sys.argv.index(name) + 1]
    return os.environ.get("CHANNEL_KIT_" + name.strip("-").upper(), default)


folder = os.environ.get("CHANNEL_KIT_FOLDER") or sys.argv[1]
if os.path.exists(os.path.join(folder, "claude-code", ".darkride-mcp.json")):
    folder = os.path.join(folder, "claude-code")
mode = option("--mode", "check")
line = option("--line", "none")
version = option("--version", "2.1.319")
seconds = float(option("--seconds", "10"))
log = option("--log", "")   # the test client's log, for --line edited: the pass needs Make's own refusal line in it
log_start = os.path.getsize(log) if log and os.path.exists(log) else 0

if line in ("chat", "plain", "edited") and not os.environ.get("CHANNEL_KIT_RERUN"):
    # become a process whose arguments are Make's line (argv[0] "claude" too): the app reads them from the system
    # the app's line check allows only Make's options (security's MUST 21:22 11-07), so python's own -I and -c can't
    # be on the line: the script runs by path, the first operand (prompt text to the check, never a subcommand name),
    # and -I's isolation comes from the environment instead (no PYTHONPATH or PYTHONHOME, no user site, safe path)
    env = dict(os.environ, CHANNEL_KIT_RERUN="1", CHANNEL_KIT_FOLDER=folder, CHANNEL_KIT_MODE=mode,
               CHANNEL_KIT_LINE=line, CHANNEL_KIT_VERSION=version, CHANNEL_KIT_SECONDS=str(seconds),
               CHANNEL_KIT_LOG=log, PYTHONSAFEPATH="1", PYTHONNOUSERSITE="1")
    for name in ("PYTHONPATH", "PYTHONHOME", "PYTHONSTARTUP", "PYTHONEXECUTABLE"):
        env.pop(name, None)
    argv = ["claude", os.path.abspath(sys.argv[0]),
            "--restricted", "--strict-mcp-config", "--mcp-config", os.path.join(folder, ".darkride-mcp.json"),
            "--settings", os.path.join(folder, ".claude", "settings.json"), "--tools", ""]
    if line in ("chat", "edited"):
        argv += [CHANNEL_FLAG, CHANNEL_ENTRY]
    if line == "edited":
        argv += shlex.split(os.environ.get("CHANNEL_KIT_EXTRA") or "--add-dir /tmp")
    os.execve(sys.executable, argv, env)

if os.environ.get("CHANNEL_KIT_RERUN"):
    folder = os.environ["CHANNEL_KIT_FOLDER"]
    mode = os.environ["CHANNEL_KIT_MODE"]
    line = os.environ["CHANNEL_KIT_LINE"]
    version = os.environ["CHANNEL_KIT_VERSION"]
    seconds = float(os.environ["CHANNEL_KIT_SECONDS"])

config = json.load(open(os.path.join(folder, ".darkride-mcp.json")))["mcpServers"]["darkride"]
helper = subprocess.Popen([config["command"]] + config["args"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
failures = 0
next_id = [0]
events = []


def say(*parts):
    print("CHANNEL KIT", *parts)
    sys.stdout.flush()


def read_line(timeout):
    ready, _, _ = select.select([helper.stdout], [], [], timeout)
    if not ready:
        return None
    raw = helper.stdout.readline()
    if not raw:
        raise SystemExit("the helper closed: " + helper.stderr.read().decode()[-400:])
    return json.loads(raw)


def ask(method, params=None):
    next_id[0] += 1
    message = {"jsonrpc": "2.0", "id": next_id[0], "method": method, "params": params or {}}
    helper.stdin.write((json.dumps(message) + "\n").encode())
    helper.stdin.flush()
    deadline = time.time() + 20
    while time.time() < deadline:
        answer = read_line(1.0)
        if answer is None:
            continue
        if answer.get("id") == next_id[0]:
            return answer
        if answer.get("method") == "notifications/claude/channel":
            events.append(answer)
    raise SystemExit("no answer to " + method)


def tool(name, arguments=None):
    result = ask("tools/call", {"name": name, "arguments": arguments or {}})["result"]
    texts = [c["text"] for c in result["content"] if c["type"] == "text"]
    images = [c for c in result["content"] if c["type"] == "image"]
    return {"error": result.get("isError", False), "code": (result.get("structuredContent") or {}).get("code"),
            "text": texts[0] if texts else "", "images": images}


def check(name, ok, detail=""):
    global failures
    if not ok:
        failures += 1
    say(name, "PASS" if ok else "FAIL", detail)


if line == "edited":
    # the app judges the line as the helper connects and drops it (the dock shows the refused words): the helper
    # closes, or nothing answers; an initialize result would mean the edited line was taken
    say("line edited:", " ".join(sys.argv[1:]))
    try:
        answer = ask("initialize", {"protocolVersion": "2026-07-28",
                                    "clientInfo": {"name": "make-channel-kit", "version": version}})
        taken = "result" in answer
        detail = json.dumps(answer)[:200]
    except (SystemExit, OSError) as ended:
        taken = False
        detail = str(ended)[:200]
    check("the edited line was refused: the helper was dropped before any answer", not taken, detail)
    # no answer alone would also pass on a link that's down (security 21:5x 11-07): with --log <the test client's
    # log> the pass needs Make's own refusal line written in this run, and the dock's refused words are the dock
    # kit's to read and print
    if log:
        refusal = ""
        deadline = time.time() + 10
        while not refusal and time.time() < deadline:
            with open(log, "rb") as handle:
                handle.seek(log_start)
                for row in handle.read().decode("utf-8", "replace").splitlines():
                    if "Make refused a Claude Code started with a different line" in row:
                        refusal = row.strip()
            if not refusal:
                time.sleep(0.5)
        check("the client log says Make refused the line", bool(refusal), refusal[:300] or "no such line within 10 s")
    else:
        say("no --log given: Make's refusal line and the dock's refused words are the dock kit's to show")
    say("PASS" if failures == 0 else "FAIL %d" % failures)
    try:
        helper.stdin.close()
        helper.wait(5)
    except (OSError, subprocess.TimeoutExpired):
        helper.kill()
    sys.exit(1 if failures else 0)

init = ask("initialize", {"protocolVersion": "2026-07-28",
                          "clientInfo": {"name": "make-channel-kit", "version": version}})["result"]
say("initialize reply:", json.dumps({k: v for k, v in init.items() if k != "instructions"}))
check("protocol held at 2025-11-25", init.get("protocolVersion") == "2025-11-25", init.get("protocolVersion"))
experimental = (init.get("capabilities") or {}).get("experimental") or {}
check("claude/channel declared, nothing else", list(experimental.keys()) == ["claude/channel"]
      and experimental["claude/channel"] == {}, json.dumps(experimental))
check("no permission relay", "claude/channel/permission" not in experimental)
check("instructions name the channel", "<channel source=\"darkride\"" in init.get("instructions", ""))
helper.stdin.write((json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n").encode())
helper.stdin.flush()

tools = {t["name"]: t for t in ask("tools/list")["result"]["tools"]}
reply_props = tools.get("reply", {}).get("inputSchema", {}).get("properties", {})
picture_props = tools.get("picture", {}).get("inputSchema", {}).get("properties", {})
check("reply takes msg_id", "msg_id" in reply_props and "text" in reply_props, json.dumps(sorted(reply_props)))
check("picture takes reference", "reference" in picture_props, json.dumps(sorted(picture_props)))

if mode == "check":
    shown = tool("reply", {"text": "Channel kit: a reply with a msg_id.", "msg_id": "1"})
    check("reply with msg_id shown", not shown["error"], shown["text"][:120])
    shown_plain = tool("reply", {"text": "Channel kit: a reply without one."})
    check("reply without msg_id shown", not shown_plain["error"], shown_plain["text"][:120])
    bad_id = tool("reply", {"text": "x", "msg_id": "not-a-number-at-all"})
    check("a msg_id that isn't Make's is refused by the schema", bad_id["error"] and bad_id["code"] == "bad_input",
          bad_id["text"][:120])
    nobody = tool("picture", {"reference": "9"})
    check("a reference nobody attached is not_found", nobody["error"] and nobody["code"] == "not_found", nobody["text"][:120])
    say("listening %.0f s for stray events" % seconds)
    deadline = time.time() + seconds
    while time.time() < deadline:
        message = read_line(0.5)
        if message is not None and message.get("method") == "notifications/claude/channel":
            events.append(message)
    check("no channel event without a Send", len(events) == 0, "%d event(s)" % len(events))
else:
    # chat or silent: the other end of the dock's Send, for --seconds
    say("mode", mode, "line", line, "version", version, "taking events for %.0f s" % seconds)
    deadline = time.time() + seconds
    handled = 0
    while time.time() < deadline:
        message = read_line(0.5)
        if message is None or message.get("method") != "notifications/claude/channel":
            continue
        params = message.get("params") or {}
        meta = params.get("meta") or {}
        content = params.get("content", "")
        say("event msg_id=%s pictures=%s content=%r" % (meta.get("msg_id"), meta.get("pictures"), content[:200]))
        events.append(message)
        if mode == "silent":
            continue
        pictures = [p for p in (meta.get("pictures") or "").split(",") if p]
        if pictures:
            picture = tool("picture", {"reference": pictures[0]})
            say("picture reference %s: %s" % (pictures[0], "an image, %d chars" % len(picture["images"][0]["data"])
                                              if picture["images"] else picture["text"][:120]))
        shown = tool("reply", {"msg_id": meta.get("msg_id", ""),
                               "text": "Kit reply to message %s: %s" % (meta.get("msg_id"), content[:80])})
        say("reply to msg_id %s: %s" % (meta.get("msg_id"), "shown" if not shown["error"] else shown["text"][:120]))
        handled += 1
    say("events %d, answered %d" % (len(events), handled))

print("CHANNEL KIT", "PASS" if failures == 0 else "FAIL %d" % failures)
helper.stdin.close()
helper.wait(5)
sys.exit(1 if failures else 0)
