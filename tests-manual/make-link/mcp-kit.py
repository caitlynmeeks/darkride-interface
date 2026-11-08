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
#  Where Make keeps its files is the person's choice (scene's here.filesGoTo, from 11-08): "my_files", their own My
#  Files (the default; it needs a signed-in client, and the kit's walled-off clients never sign in), or "world", the
#  world's own files (a world on a server whose owner lets the person add files). Where Make can't keep files now (here.
#  canKeepFiles false: signed out, since the world's files are used only where the world allows them and My Files
#  otherwise; Make refuses a long value with its plain limit line instead of keeping it as a file), a set script or an
#  uploaded model can't be held: those checks print SKIP with Make's reason and the end line counts them, and Make's
#  answer must be its own not-saved line (signed out: "isn't signed in"). The driver says which world it is with
#  --world-files yes|no (no: the serverless file:// world, where a server script is refused before it is saved) and
#  whether the client is signed in with --signed-in yes|no (default no); Make's scene must agree.
#    python3 -I mcp-kit.py <the test client's Make folder> [<folder for the made model's views>] [--world-files yes|no]
#        [--signed-in yes|no]
#  (the MCP line is in the Make folder's claude-code/, where Connect writes it)
#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#

import base64, json, os, subprocess, sys, time

arguments = sys.argv[1:]
declared = None   # --world-files yes|no: which world the driver started the client in
if "--world-files" in arguments:
    at = arguments.index("--world-files")
    declared = {"yes": True, "no": False}.get(arguments[at + 1] if at + 1 < len(arguments) else "")
    if declared is None:
        raise SystemExit("--world-files takes yes or no")
    del arguments[at:at + 2]
signed_in = False   # --signed-in yes|no: the kit's walled-off clients never sign in
if "--signed-in" in arguments:
    at = arguments.index("--signed-in")
    signed_in = {"yes": True, "no": False}.get(arguments[at + 1] if at + 1 < len(arguments) else "")
    if signed_in is None:
        raise SystemExit("--signed-in takes yes or no")
    del arguments[at:at + 2]
folder = arguments[0]
if os.path.exists(os.path.join(folder, "claude-code", ".darkride-mcp.json")):
    folder = os.path.join(folder, "claude-code")
out = arguments[1] if len(arguments) > 1 else os.path.dirname(os.path.abspath(sys.argv[0]))
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


skipped = 0
NO_FILES = "Make can't keep files here now (scene's here.canKeepFiles false)"
# Make's own not-saved lines (MakeLink): the world's files refused, or none, or the person signed out with My Files
NOT_SAVED_LINES = ("keeps no files", "may not add files", "world's files", "isn't signed in to Darkride")


def skip(name, why):
    global skipped
    skipped += 1
    print("MCP KIT", name, "SKIP", why)
    sys.stdout.flush()


init = ask("initialize", {"protocolVersion": "2025-11-25", "clientInfo": {"name": "make-mcp-kit", "version": "1"}})
check("initialize", init["result"]["serverInfo"]["name"] == "darkride")
names = [t["name"] for t in ask("tools/list")["result"]["tools"]]
check("tools offered", "scene" in names and "make_with_blender" in names, str(len(names)) + " tools")

guide = tool("guide")
check("guide", not guide["error"] and len(guide["value"]["text"]) > 2000)
scene = tool("scene", {"radius": 30})
check("scene", not scene["error"] and "here" in scene["value"], json.dumps(scene["value"]["here"]) if scene["value"] else scene["text"])
# Make says up front where files go and whether it can keep one now (here.filesGoTo, here.canKeepFiles, from 11-08);
# with the world's files that must fit the declared world, with My Files the declared sign-in
here = ((scene["value"] or {}).get("here") or {}) if not scene["error"] else {}
scene_files = here.get("canKeepFiles")
place = here.get("filesGoTo")
if place is not None:
    check("scene says where Make keeps files (filesGoTo %s)" % place, place in ("my_files", "world"))
if place == "my_files" and scene_files is not None:
    check("scene says My Files can keep files only when signed in (canKeepFiles %s, signed in %s)"
          % (scene_files, "yes" if signed_in else "no"), scene_files == signed_in)
elif declared is not None and scene_files is not None:
    check("scene says the world's files as declared (canKeepFiles %s)" % scene_files, scene_files == declared)

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
# a refusal naming the limit comes only where the world can't keep files (MakeLink: the files route needs the
# world's Asset Server): that tells the kit which world it is in for the checks below
refused_at_limit = material["error"] and material["code"] == "bad_input" and "at most 1200 bytes" in material["text"]
if scene_files is not None:
    world_files = scene_files   # from here on: Make can keep a file now (where the person chose)
    check("Make's answer fits its scene (keeps files %s)" % ("yes" if scene_files else "no"),
          refused_at_limit != scene_files, material["text"][:160])
elif declared is None:
    world_files = not refused_at_limit
    print("MCP KIT world files: inferred %s from Make's answer (give --world-files yes|no to check it)"
          % ("yes" if world_files else "no"))
else:
    world_files = declared
    check("Make's answer fits the world (files %s)" % ("yes" if declared else "no"), refused_at_limit != declared,
          material["text"][:160])


def kept_link(url, folder):
    # where the person chose: the world's own files (atp:/claude-<folder>/) or My Files' lasting link (files host /f/)
    if place == "my_files":
        return url.startswith("https://files.") and "/f/" in url
    return url.startswith("atp:/claude-" + folder + "/")


if world_files:
    check("a long materialData is added", not material["error"], material["text"][:200])
else:
    skip("a long materialData is added", NO_FILES)
    check("...refused instead with the plain limit line", True, material["text"][:120])
if not material["error"]:
    time.sleep(1)
    held = ((tool("get", {"id": material["value"]["id"]})["value"] or {}).get("properties") or {})
    check("...kept as a file, the Material pointing at it",
          kept_link(str(held.get("materialURL", "")), "materials") and len(held.get("materialData", "")) <= 1200,
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
if world_files:
    check("set_script", not script["error"] and kept_link(script["value"]["url"], "scripts")
          and os.path.exists(script["value"]["saved_copy"] or ""), script["text"][:300])
else:
    skip("set_script", NO_FILES + ("" if script["error"] and any(line in script["text"] for line in NOT_SAVED_LINES)
                                   else "; but Make's answer wasn't its not-saved line: " + script["text"][:160]))
    if place == "my_files" and not signed_in:
        check("...Make says the person isn't signed in (My Files)", script["error"]
              and "isn't signed in to Darkride" in script["text"] and script["text"].startswith("Not saved"),
              script["text"][:200])
if not script["error"]:
    copy = open(script["value"]["saved_copy"]).read()
    check("script marked made with Claude", copy.startswith("// made with Claude"))
    if place == "my_files":
        # anyone with a My Files link may read it: the person's name stays out of the stamp (pm 11-08)
        check("...without the person's name (My Files)", " for " not in copy.splitlines()[0], copy.splitlines()[0])
if declared is False:
    # the file world (--world-files no): it has no server, so a server script is refused before it is saved
    server = tool("set_script", {"id": box_id, "side": "server", "source": "(function () {})"})
    check("a server script in a file world is refused (no server)", server["error"]
          and "server scripts need a world on a server" in server["text"] and server["text"].startswith("Not saved"),
          server["text"][:200])
time.sleep(2)
clicked = tool("click", {"id": box_id})
check("click", not clicked["error"] and clicked["value"]["clicked"])
time.sleep(1)
log = tool("log")
if world_files:
    check("log has the click", not log["error"] and any("MAKE KIT CLICKED" in line for line in log["value"]["client"]),
          log["text"][:300])
else:
    skip("log has the click", NO_FILES + ": the click script was never set")
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
    if world_files:
        check("upload_model", not uploaded["error"] and kept_link(uploaded["value"]["url"], "models"),
              uploaded["text"][:200])
    else:
        skip("upload_model", NO_FILES + ("" if uploaded["error"] and any(line in uploaded["text"] for line in NOT_SAVED_LINES)
                                         else "; but Make's answer wasn't its not-saved line: " + uploaded["text"][:160]))
    if not uploaded["error"]:
        model = tool("add", {"properties": {"type": "Model", "name": "Make kit stool", "modelURL": uploaded["value"]["url"],
                                            "dimensions": {"x": 0.4, "y": 0.45, "z": 0.4}}})
        check("place the model", not model["error"])

picture = tool("picture", {"id": box_id})
check("picture", not picture["error"] and len(picture["images"]) == 1, picture["text"][:200])
walked = tool("walk", {"id": box_id})
check("walk", not walked["error"] and walked["value"]["arrived"], walked["text"][:200])
undone = tool("undo", {"steps": 50})
# with files: the box, its material, its script and the edit at least; without: the box and the edit
check("undo", not undone["error"] and len(undone["value"]["undone"]) >= (3 if world_files else 2), undone["text"][:300])
gone = tool("scene", {"radius": 30, "name": "Make kit"})
check("undo took them all back", not gone["error"] and len(gone["value"]["things"]) == 0, gone["text"][:300])
# the kit client's script quits when it sees this (a throwaway world)
tool("add", {"properties": {"type": "Box", "name": "Make kit done", "dimensions": {"x": 0.1, "y": 0.1, "z": 0.1}}})

if skipped:
    print("MCP KIT skipped %d: %s%s" % (skipped, NO_FILES, "" if declared is not None else " (inferred)"))
print("MCP KIT", "PASS" if failures == 0 else "FAIL %d" % failures)
helper.stdin.close()
helper.wait(5)
sys.exit(1 if failures else 0)
