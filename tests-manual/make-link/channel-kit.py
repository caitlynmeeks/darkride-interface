#!/usr/bin/env python3
#
#  channel-kit.py
#  tests-manual/make-link
#
#  Make's channel to Claude Code, from the Claude Code side, with no Claude and nothing spent: a scripted MCP client
#  speaks to a running test client's Make through the darkride-mcp helper (as Claude Code started with Make's chat
#  line would) and checks what a channel client must see: the initialize reply (the protocol held at 2025-11-25 when
#  2026-07-28 is asked, "claude/channel" the only experimental capability, never the permission relay), the reply tool
#  taking msg_id and the picture tool taking reference, a reply by msg_id shown, a reference nobody attached refused
#  as not_found, and no channel event arriving unprompted (nothing but the dock's Send makes one). Prints one line per
#  check and "CHANNEL KIT PASS" or "CHANNEL KIT FAIL n".
#    python3 -I channel-kit.py <the test client's Make folder> [<seconds to listen for stray events, default 10>]
#  The dock's own Send (kit 1 of the plan) and "only the builder's typing" (kit 2) run in editor-ui's dock kit, which
#  reads MakeLink::channelEventCount().
#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#

import json, os, select, subprocess, sys, time

folder = sys.argv[1]
if os.path.exists(os.path.join(folder, "claude-code", ".darkride-mcp.json")):
    folder = os.path.join(folder, "claude-code")
listen_seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
config = json.load(open(os.path.join(folder, ".darkride-mcp.json")))["mcpServers"]["darkride"]
helper = subprocess.Popen([config["command"]] + config["args"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
failures = 0
next_id = [0]
events = []


def read_line(timeout):
    ready, _, _ = select.select([helper.stdout], [], [], timeout)
    if not ready:
        return None
    line = helper.stdout.readline()
    if not line:
        raise SystemExit("the helper closed: " + helper.stderr.read().decode()[-400:])
    return json.loads(line)


def ask(method, params=None):
    next_id[0] += 1
    message = {"jsonrpc": "2.0", "id": next_id[0], "method": method, "params": params or {}}
    helper.stdin.write((json.dumps(message) + "\n").encode())
    helper.stdin.flush()
    deadline = time.time() + 10
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
    return {"error": result.get("isError", False), "code": (result.get("structuredContent") or {}).get("code"),
            "text": texts[0] if texts else ""}


def check(name, ok, detail=""):
    global failures
    if not ok:
        failures += 1
    print("CHANNEL KIT", name, "PASS" if ok else "FAIL", detail)
    sys.stdout.flush()


init = ask("initialize", {"protocolVersion": "2026-07-28",
                          "clientInfo": {"name": "make-channel-kit", "version": "2.1.319"}})["result"]
print("CHANNEL KIT initialize reply:", json.dumps({k: v for k, v in init.items() if k != "instructions"}))
check("protocol held at 2025-11-25", init.get("protocolVersion") == "2025-11-25", init.get("protocolVersion"))
experimental = (init.get("capabilities") or {}).get("experimental") or {}
check("claude/channel declared, nothing else", list(experimental.keys()) == ["claude/channel"]
      and experimental["claude/channel"] == {}, json.dumps(experimental))
check("no permission relay", "claude/channel/permission" not in experimental)
check("instructions name the channel", "<channel source=\"darkride\"" in init.get("instructions", ""))

tools = {t["name"]: t for t in ask("tools/list")["result"]["tools"]}
reply_props = tools.get("reply", {}).get("inputSchema", {}).get("properties", {})
picture_props = tools.get("picture", {}).get("inputSchema", {}).get("properties", {})
check("reply takes msg_id", "msg_id" in reply_props and "text" in reply_props, json.dumps(sorted(reply_props)))
check("picture takes reference", "reference" in picture_props, json.dumps(sorted(picture_props)))

shown = tool("reply", {"text": "Channel kit: a reply with a msg_id.", "msg_id": "1"})
check("reply with msg_id shown", not shown["error"], shown["text"][:120])
shown_plain = tool("reply", {"text": "Channel kit: a reply without one."})
check("reply without msg_id shown", not shown_plain["error"], shown_plain["text"][:120])
bad_id = tool("reply", {"text": "x", "msg_id": "not-a-number-at-all"})
check("a msg_id that isn't Make's is refused by the schema", bad_id["error"] and bad_id["code"] == "bad_input",
      bad_id["text"][:120])
nobody = tool("picture", {"reference": "9"})
check("a reference nobody attached is not_found", nobody["error"] and nobody["code"] == "not_found", nobody["text"][:120])

# nothing but the dock's Send makes an event: listen a while
print("CHANNEL KIT listening %.0f s for stray events" % listen_seconds)
sys.stdout.flush()
deadline = time.time() + listen_seconds
while time.time() < deadline:
    message = read_line(0.5)
    if message is not None and message.get("method") == "notifications/claude/channel":
        events.append(message)
check("no channel event without a Send", len(events) == 0, "%d event(s)" % len(events))

print("CHANNEL KIT", "PASS" if failures == 0 else "FAIL %d" % failures)
helper.stdin.close()
helper.wait(5)
sys.exit(1 if failures else 0)
