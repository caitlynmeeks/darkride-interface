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
#  Mode "runaway" only loops, for the kill-the-parent case.
#    blender --background --factory-startup --disable-autoexec -noaudio --python probe.py --
#            <job folder> <probe|runaway> <profile folder> <test folder>
#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#

import ctypes, json, os, socket, subprocess, sys, time

job, mode, profile, outside = sys.argv[sys.argv.index("--") + 1:][:4]
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


for folder in ("Documents", "Desktop", ".ssh", os.path.join("AppData", "Roaming"), os.path.join("AppData", "Local")):
    path = os.path.join(profile, folder)
    refused("open " + folder, lambda path=path: open_folder(path))
targets = json.load(open(os.path.join(job, "targets.json")))  # {"loopback": [address, port], "lan": [address, port]}
# loopback: the test program's own listener there; any failure is the box's refusal
refused("connect to the test's own loopback listener",
        lambda: socket.create_connection(tuple(targets["loopback"]), timeout=5).close())
# the LAN: a closed port on this laptop's own LAN address (no listener, so no firewall prompt and no other machine).
# Outside a box that's "connection refused" (the laptop answered); the box refuses it itself (access denied)
try:
    socket.create_connection(tuple(targets["lan"]), timeout=5).close()
    results.append({"check": "connect to this laptop's LAN address", "pass": False, "detail": "opened"})
except ConnectionRefusedError:
    results.append({"check": "connect to this laptop's LAN address", "pass": False, "detail": "reached the laptop (not boxed)"})
except Exception as e:  # noqa: BLE001
    results.append({"check": "connect to this laptop's LAN address", "pass": True, "detail": "refused (%s %s)" % (
        type(e).__name__, getattr(e, "winerror", None) or getattr(e, "errno", ""))})
refused("DNS lookup", lambda: socket.getaddrinfo("darkride-box-test.invalid", 80))
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
