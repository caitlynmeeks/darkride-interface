#
#  probe.py
#  tests-manual/make-link/windows-box
#
#  Runs inside Blender inside make-box-test.exe's box (an AppContainer with no capabilities, in a job object) and
#  checks what the box refuses: the person's folders (opened and closed at once: no bytes read, no names kept), the
#  network (the test program's own loopback listener, a closed port on this laptop's LAN address, and a DNS name
#  under .invalid: no outside service, no firewall prompt), a write outside the job folder, starting another program, reading the test
#  program's own throwaway Credential Manager entry by its exact name, and that no key is in its environment. Then it
#  renders four views of a test model, on the GPU (EEVEE) and on the CPU (Cycles). It writes probe-results.json in the
#  job folder: each check's refused/opened and an exception type only, never a path or a message (security, 11-07).
#  Mode "start" only says it started (the UI-limit ladder); mode "runaway" only loops, for the kill-the-parent case.
#  Mode "net-control" (run 6) runs only the network checks, UNBOXED, and writes net-control.json: there each check
#  passes when it reaches its target, which shows the boxed refusal of the same check means something.
#  Network targets (targets.json): the test's loopback listener, a closed port on the LAN address, the laptop's own
#  computer name, a name under .invalid, and when the test was given them: a listener on the Mac, Windows' own port
#  135 on the LAN address (a bare connect, closed at once, nothing sent or read), a listener of the test's own on the
#  LAN address, and (run 7) a public name given with --public-name (resolved or not, nothing kept). The closed port
#  and the test's own listener come again on the laptop's own IPv6 address when it has one (lan6_closed,
#  lan6_listener; security's second-machine item). Details keep an exception's type and number only, never an address
#  or a message.
#  Scoring (security, 11-07 after run 6): the closed port, the own computer name and the public name are INFO in the
#  box ("pass": null, "score": "info"): their reached/not is recorded, never a pass or a fail. A closed port's RST is
#  the laptop's own network stack with nothing listening, and whether names may resolve in the box is Caity's call.
#    blender --background --factory-startup --disable-autoexec -noaudio --python probe.py --
#            <job folder> <probe|net-control|start|runaway> <profile folder> <test folder>
#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#

import ctypes, json, os, socket, subprocess, sys, time

job, mode, profile, outside = sys.argv[sys.argv.index("--") + 1:][:4]
if mode == "start":  # the start ladder: only that Blender got this far
    open(os.path.join(job, "started.txt"), "w").write("started")
    raise SystemExit(0)
if mode == "runaway":
    open(os.path.join(job, "runaway-started.txt"), "w").write(str(os.getpid()))
    while True:
        pass

results = []


def refused(name, action):
    """The box must refuse the action: an exception passes, success fails. Only the exception's type is kept."""
    try:
        action()
        results.append({"check": name, "pass": False, "detail": "opened"})
    except FileNotFoundError:
        results.append({"check": name, "pass": None, "detail": "absent"})
    except Exception as e:  # noqa: BLE001 - any refusal counts
        results.append({"check": name, "pass": True, "detail": "refused (%s)" % type(e).__name__})


def open_folder(path):
    with os.scandir(path):  # opened and closed at once: nothing listed, nothing read
        pass


targets = json.load(open(os.path.join(job, "targets.json")))
# {"token": str, "loopback", "lan_closed", "lan_listener", "lan6_closed", "lan6_listener", "system_port",
#  "mac": [address, port] or null, "public_name": str or null}


def failure(e):
    """An exception's type and number only (never its message, which can name an address)."""
    return "%s %s" % (type(e).__name__, getattr(e, "winerror", None) or getattr(e, "errno", None) or "")


def connect(address, greeting=None, read=True):
    """(reached, detail). Reached: the connection opened, or the target's machine answered "refused" (an RST, so the
    attempt got through to its network stack). Not reached: anything else (access denied, timed out, unreachable)."""
    try:
        with socket.create_connection(tuple(address), timeout=5) as s:
            if not read:  # Windows' own port 135: nothing sent, nothing read, closed at once
                return True, "connected"
            s.settimeout(5)
            try:
                got = s.recv(64)
            except OSError:
                got = b""
            if greeting is None:
                return True, "connected"
            return True, "connected, " + ("greeting ok" if got.startswith(greeting) else "no greeting")
    except ConnectionRefusedError as e:
        return True, "answered refused (%s)" % failure(e)
    except Exception as e:  # noqa: BLE001 - any other failure is not reaching it
        return False, failure(e)


def resolve(name):
    """name: a name, or None for this laptop's own computer name (looked up inside the try: the box may refuse that)."""
    try:
        socket.getaddrinfo(socket.gethostname() if name is None else name, 80)
        return True, "resolved"  # pass/fail only: no address kept
    except Exception as e:  # noqa: BLE001
        return False, failure(e)


def network_checks(boxed):
    """Each check: reached or not. Boxed, a check passes when it did NOT reach. In the unboxed control it passes when it
    shows the target is there: a listener (ours, the Mac's, Windows' port 135) must CONNECT, the closed port must be
    answered, the own name must resolve; only then does the boxed refusal of the same check mean something (security:
    otherwise it's unknown). A name under .invalid fails unboxed too: no control (pass null)."""
    token = targets["token"].encode()
    # (name, action, what the control needs: "connected", "reached", or None for no control)
    checks = [("the test's own loopback listener", lambda: connect(targets["loopback"], token), "connected"),
              ("a closed port on this laptop's LAN address", lambda: connect(targets["lan_closed"], None, False), "reached"),
              ("this laptop's own computer name (DNS)", lambda: resolve(None), "reached"),
              ("a name under .invalid (DNS)", lambda: resolve("darkride-box-test.invalid"), None)]
    if targets.get("lan_listener"):
        checks.append(("the test's own listener on this laptop's LAN address",
                       lambda: connect(targets["lan_listener"], token), "connected"))
    if targets.get("lan6_closed"):
        checks.append(("a closed port on this laptop's IPv6 address",
                       lambda: connect(targets["lan6_closed"], None, False), "reached"))
    if targets.get("lan6_listener"):
        checks.append(("the test's own listener on this laptop's IPv6 address",
                       lambda: connect(targets["lan6_listener"], token), "connected"))
    if targets.get("system_port"):
        checks.append(("Windows' own port 135 on this laptop's LAN address",
                       lambda: connect(targets["system_port"], None, False), "connected"))
    if targets.get("mac"):
        # the Mac's listener only accepts and closes: a bare connect, nothing sent or read
        checks.append(("the Mac's listener (off the laptop)", lambda: connect(targets["mac"], None, False), "connected"))
    if targets.get("public_name"):
        checks.append(("a public name (DNS)", lambda: resolve(targets["public_name"]), "reached"))
    # recorded, not scored, in the box (security, 11-07 after run 6; see the top)
    info = ("a closed port on this laptop's LAN address", "a closed port on this laptop's IPv6 address",
            "this laptop's own computer name (DNS)", "a public name (DNS)")
    for name, action, control_needs in checks:
        reached, detail = action()
        if boxed:
            passed = None if name in info else not reached
        elif control_needs == "connected":
            passed = reached and detail.startswith("connected")
        elif control_needs == "reached":
            passed = reached
        else:
            passed = None
        entry = {"check": "network: " + name, "reached": reached, "pass": passed, "detail": detail}
        if boxed and name in info:
            entry["score"] = "info"
        results.append(entry)


if mode == "net-control":
    network_checks(boxed=False)
    json.dump({"results": results}, open(os.path.join(job, "net-control.json"), "w"), indent=1)
    raise SystemExit(0)

for folder in ("Documents", "Desktop", ".ssh", os.path.join("AppData", "Roaming"), os.path.join("AppData", "Local")):
    path = os.path.join(profile, folder)
    refused("open " + folder, lambda path=path: open_folder(path))
network_checks(boxed=True)
refused("write outside the job folder", lambda: open(os.path.join(outside, "written-from-the-box.txt"), "w").write("x"))
refused("start another program", lambda: subprocess.run(["cmd.exe", "/c", "echo", "hi"], check=True, timeout=10))


class CREDENTIAL(ctypes.Structure):
    _fields_ = [("Flags", ctypes.c_uint32), ("Type", ctypes.c_uint32), ("TargetName", ctypes.c_wchar_p),
                ("Comment", ctypes.c_wchar_p), ("LastWritten", ctypes.c_uint64), ("CredentialBlobSize", ctypes.c_uint32),
                ("CredentialBlob", ctypes.c_void_p), ("Persist", ctypes.c_uint32), ("AttributeCount", ctypes.c_uint32),
                ("Attributes", ctypes.c_void_p), ("TargetAlias", ctypes.c_wchar_p), ("UserName", ctypes.c_wchar_p)]


def read_credential():
    found = ctypes.POINTER(CREDENTIAL)()
    if not ctypes.windll.advapi32.CredReadW("Darkride Make box test - delete me", 1, 0, ctypes.byref(found)):
        raise PermissionError("CredReadW failed, error %d" % ctypes.GetLastError())
    ctypes.windll.advapi32.CredFree(found)


refused("read the Credential Manager entry", read_credential)
leaked = [k for k, v in os.environ.items() if "ANTHROPIC" in k.upper() or "kit-test-env-key" in v]
results.append({"check": "no key in the environment", "pass": not leaked, "detail": "%d found" % len(leaked)})

# the four check views, on the GPU and on the CPU
import bpy  # noqa: E402

bpy.ops.mesh.primitive_monkey_add(location=(0, 0, 0))
bpy.ops.object.light_add(type="SUN", location=(3, -3, 5))
bpy.ops.object.camera_add(location=(0, -4, 1.5), rotation=(1.35, 0, 0))
scene = bpy.context.scene
scene.camera = bpy.context.active_object
scene.render.resolution_x = scene.render.resolution_y = 256
views = {"front": (0, -4, 1.5, 1.35, 0, 0), "right": (4, 0, 1.5, 1.35, 0, 1.5708),
         "back": (-3, 3, 1.5, 1.35, 0, -2.356), "top": (0, 0, 5, 0, 0, 0)}
engines = [e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items]
for engine in [e for e in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE") if e in engines][:1] + ["CYCLES"]:
    started = time.time()
    try:
        scene.render.engine = engine
        if engine == "CYCLES":
            scene.cycles.device = "CPU"
            scene.cycles.samples = 8
        for view, (x, y, z, rx, ry, rz) in views.items():
            scene.camera.location = (x, y, z)
            scene.camera.rotation_euler = (rx, ry, rz)
            scene.render.filepath = os.path.join(job, "%s-%s.png" % (engine.lower(), view))
            bpy.ops.render.render(write_still=True)
        made = all(os.path.getsize(os.path.join(job, "%s-%s.png" % (engine.lower(), v))) > 1000 for v in views)
        results.append({"check": "render 4 views with " + engine, "pass": made, "detail": "%.1f s" % (time.time() - started)})
    except Exception as e:  # noqa: BLE001
        results.append({"check": "render 4 views with " + engine, "pass": False,
                        "detail": "%s (%.1f s)" % (type(e).__name__, time.time() - started)})

json.dump({"blender": bpy.app.version_string, "results": results}, open(os.path.join(job, "probe-results.json"), "w"), indent=1)
