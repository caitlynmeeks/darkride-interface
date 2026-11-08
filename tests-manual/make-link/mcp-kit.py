#!/usr/bin/env python3
#
#  mcp-kit.py
#  tests-manual/make-link
#
#  Make's tools end to end, with no Claude and nothing spent: a scripted MCP client speaks to a running test client's
#  Make through the darkride-mcp helper, as Claude Code would (the Make folder's .darkride-mcp.json names the helper and
#  its socket), and checks each tool's answer: the guide, a look around, a box added and read back, a client script set
#  (its readable copy kept) and clicked (its print in the log), an edit watched, a script that uses the web asked about
#  first (the kit's client answers no), a model made in Blender, put in the world's files and placed, a picture, a walk,
#  and undo. Prints one line per check and "MCP KIT PASS" or "MCP KIT FAIL n".
#    python3 -I mcp-kit.py <the test client's Make folder> [<folder for the made model's views>]
#  (the MCP line is in the Make folder's claude-code/, where Connect writes it)
#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#

import base64, json, os, subprocess, sys, time

folder = sys.argv[1]
if os.path.exists(os.path.join(folder, "claude-code", ".darkride-mcp.json")):
    folder = os.path.join(folder, "claude-code")
out = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(os.path.abspath(sys.argv[0]))
config = json.load(open(os.path.join(folder, ".darkride-mcp.json")))["mcpServers"]["darkride"]
helper = subprocess.Popen([config["command"]] + config["args"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
failures = 0
next_id = [0]


def ask(method, params=None):
    next_id[0] += 1
    message = {"jsonrpc": "2.0", "id": next_id[0], "method": method, "params": params or {}}
    helper.stdin.write((json.dumps(message) + "\n").encode())
    helper.stdin.flush()
    while True:
        line = helper.stdout.readline()
        if not line:
            raise SystemExit("the helper closed: " + helper.stderr.read().decode()[-400:])
        answer = json.loads(line)
        if answer.get("id") == next_id[0]:
            return answer


def tool(name, arguments=None):
    started = time.time()
    result = ask("tools/call", {"name": name, "arguments": arguments or {}})["result"]
    texts = [c["text"] for c in result["content"] if c["type"] == "text"]
    images = [c for c in result["content"] if c["type"] == "image"]
    text = texts[0] if texts else ""
    body = text.split("\n", 1)[1] if text.startswith("World content:") else text
    value = None
    if not result.get("isError"):
        try:
            value = json.loads(body)
        except ValueError:
            value = None
    return {"error": result.get("isError", False), "code": (result.get("structuredContent") or {}).get("code"),
            "text": text, "value": value, "images": images, "seconds": round(time.time() - started, 1)}


def check(name, ok, detail=""):
    global failures
    if not ok:
        failures += 1
    print("MCP KIT", name, "PASS" if ok else "FAIL", detail)
    sys.stdout.flush()


init = ask("initialize", {"protocolVersion": "2025-11-25", "clientInfo": {"name": "make-mcp-kit", "version": "1"}})
check("initialize", init["result"]["serverInfo"]["name"] == "darkride")
names = [t["name"] for t in ask("tools/list")["result"]["tools"]]
check("tools offered", "scene" in names and "make_with_blender" in names, str(len(names)) + " tools")

guide = tool("guide")
check("guide", not guide["error"] and len(guide["value"]["text"]) > 2000)
scene = tool("scene", {"radius": 30})
check("scene", not scene["error"] and "here" in scene["value"], json.dumps(scene["value"]["here"]) if scene["value"] else scene["text"])

box = tool("add", {"properties": {"type": "Box", "name": "Make kit box", "dimensions": {"x": 0.5, "y": 0.5, "z": 0.5},
                                  "color": {"red": 200, "green": 40, "blue": 40}}})
check("add", not box["error"] and box["value"]["made_by"]["kind"] == "person", box["text"][:200])
box_id = box["value"]["id"] if box["value"] else ""
refused = tool("add", {"properties": {"type": "Box", "script": "(function () {})"}})
check("add refuses a script property", refused["error"] and refused["code"] == "refused")
got = tool("get", {"id": box_id})
check("get", not got["error"] and got["value"]["properties"]["name"] == "Make kit box" and got["text"].startswith("World content:"))

# a value longer than Make sets is refused with a plain line; a long material goes into the world's files instead
long_ud = '{"d":"' + "u" * 1300 + '"}'
refused_long = tool("add", {"properties": {"type": "Box", "name": "Make kit long", "userData": long_ud}})
check("a too-long userData is refused", refused_long["error"] and refused_long["code"] == "bad_input"
      and "1200 bytes" in refused_long["text"], refused_long["text"][:200])
refused_edit = tool("edit", {"id": box_id, "properties": {"userData": long_ud}})
check("a too-long edit is refused", refused_edit["error"] and refused_edit["code"] == "bad_input", refused_edit["text"][:200])
material = tool("add", {"properties": {"type": "Material", "name": "Make kit long material", "parentID": box_id,
                                       "materialData": '{"materials":{"name":"' + "m" * 5000 + '"}}'}})
check("a long materialData is added", not material["error"], material["text"][:200])
if not material["error"]:
    time.sleep(1)
    held = ((tool("get", {"id": material["value"]["id"]})["value"] or {}).get("properties") or {})
    check("...into the world's files, the Material pointing at it",
          str(held.get("materialURL", "")).startswith("atp:/claude-materials/") and len(held.get("materialData", "")) <= 1200,
          "materialURL %s, materialData %d chars" % (held.get("materialURL"), len(held.get("materialData", ""))))
# a long material naming a web address is asked about first, and on "no" nothing goes into the world's files (the
# runner counts the /claude-materials/ files in the client's log: this one adds none)
web_material = tool("add", {"properties": {"type": "Material", "name": "Make kit web material", "parentID": box_id,
                                           "materialData": '{"materials":{"albedoMap":"https://example.com/kit.png","name":"'
                                                           + "m" * 1500 + '"}}'}})
check("a long material with a web address is asked first (the kit says no)", web_material["error"]
      and web_material["code"] == "refused", web_material["text"][:200])

script = tool("set_script", {"id": box_id, "side": "client", "source":
                             "(function () { this.clickDownOnEntity = function () { print('MAKE KIT CLICKED'); }; })"})
check("set_script", not script["error"] and script["value"]["url"].startswith("atp:/claude-scripts/")
      and os.path.exists(script["value"]["saved_copy"] or ""), script["text"][:300])
if not script["error"]:
    copy = open(script["value"]["saved_copy"]).read()
    check("script marked made with Claude", copy.startswith("// made with Claude"))
time.sleep(2)
clicked = tool("click", {"id": box_id})
check("click", not clicked["error"] and clicked["value"]["clicked"])
time.sleep(1)
log = tool("log")
check("log has the click", not log["error"] and any("MAKE KIT CLICKED" in line for line in log["value"]["client"]), log["text"][:300])
# the person's own scripts stay out of the log (the kit client's script prints "MAKE KIT RUNNING" lines by then)
check("log leaves out the person's own scripts", not log["error"] and not any("MAKE KIT RUNNING" in line or "MAKE KIT CLIENT" in line
      for line in log["value"]["client"]))

edited = tool("edit", {"id": box_id, "properties": {"rotationDegrees": {"x": 0, "y": 45, "z": 0}}})
check("edit", not edited["error"])
watched = tool("watch", {"ids": [box_id], "seconds": 1})
check("watch", not watched["error"] and watched["value"]["changes"][0]["id"] == box_id)

web = tool("set_script", {"id": box_id, "side": "client", "source": "var x = new XMLHttpRequest(); x.open('GET', 'https://example.com');"})
check("a script that uses the web is asked first (the kit says no)", web["error"] and web["code"] == "refused", web["text"][:200])

blender = tool("make_with_blender", {"name": "kit stool", "script":
    "import bpy\nbpy.ops.mesh.primitive_cylinder_add(radius=0.2, depth=0.45, location=(0, 0, 0.225))\n"
    "m = bpy.data.materials.new('oak'); m.use_nodes = True\n"
    "m.node_tree.nodes['Principled BSDF'].inputs['Base Color'].default_value = (0.2, 0.1, 0.04, 1)\n"
    "bpy.context.active_object.data.materials.append(m)\n"})
check("make_with_blender", not blender["error"] and len(blender["images"]) == 1, "%s s, %s" % (blender["seconds"], blender["text"][:200]))
if not blender["error"]:
    open(os.path.join(out, "results-kit-views.jpg"), "wb").write(
        base64.b64decode(blender["images"][0]["data"]))
    uploaded = tool("upload_model", {"model_id": blender["value"]["model_id"], "name": "kit stool"})
    check("upload_model", not uploaded["error"] and uploaded["value"]["url"].startswith("atp:/claude-models/"), uploaded["text"][:200])
    if not uploaded["error"]:
        model = tool("add", {"properties": {"type": "Model", "name": "Make kit stool", "modelURL": uploaded["value"]["url"],
                                            "dimensions": {"x": 0.4, "y": 0.45, "z": 0.4}}})
        check("place the model", not model["error"])

picture = tool("picture", {"id": box_id})
check("picture", not picture["error"] and len(picture["images"]) == 1, picture["text"][:200])
walked = tool("walk", {"id": box_id})
check("walk", not walked["error"] and walked["value"]["arrived"], walked["text"][:200])
undone = tool("undo", {"steps": 50})
check("undo", not undone["error"] and len(undone["value"]["undone"]) >= 3, undone["text"][:300])
gone = tool("scene", {"radius": 30, "name": "Make kit"})
check("undo took them all back", not gone["error"] and len(gone["value"]["things"]) == 0, gone["text"][:300])
# the kit client's script quits when it sees this (a throwaway world)
tool("add", {"properties": {"type": "Box", "name": "Make kit done", "dimensions": {"x": 0.1, "y": 0.1, "z": 0.1}}})

print("MCP KIT", "PASS" if failures == 0 else "FAIL %d" % failures)
helper.stdin.close()
helper.wait(5)
sys.exit(1 if failures else 0)
