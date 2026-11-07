#
#  probe.py
#  tests-manual/make-link/linux-box
#
#  Runs inside Blender, started by make-box-test (Make 6c, Linux). Checks what the box refuses, then renders. Modes:
#    start    the start ladder: one tiny Cycles image, then "rendered" in started.txt
#    runaway  loops forever (the CPU-cap and kill-the-parent cases)
#    control  UNBOXED, the box's environment: every check below, where "opened" shows the target is there, so the
#             box's refusal of the same check means something (security, 11-07); one small view per render engine
#    probe    in the box: every check (opened = a hole), four views with EEVEE and with Cycles on the CPU, then the
#             two exec checks, last, since a working exec would end this process
#  Each file check opens and closes at once: no bytes read, nothing listed, no names kept. Each network check is a
#  bare connect or a token the test program itself sent. Details keep an exception's type and number only, never a
#  path, an address or a message. Results: <job>/<mode>-results.json, and in probe mode <job>/probe-summary.txt.
#    blender --background --factory-startup --disable-autoexec -noaudio --python-exit-code 3 --python probe.py --
#            <job folder> <start|runaway|control|probe>
#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#

import ctypes, json, os, socket, subprocess, sys, time

job, mode = sys.argv[sys.argv.index("--") + 1:][:2]

VIEWS = {"front": (0, -4, 1.5, 1.35, 0, 0), "right": (4, 0, 1.5, 1.35, 0, 1.5708),
         "back": (-3, 3, 1.5, 1.35, 0, -2.356), "top": (0, 0, 5, 0, 0, 0)}
scene = None


def failure(e):
    """An exception's type and number only (never its message, which can name a path or an address)."""
    number = getattr(e, "errno", None)
    return type(e).__name__ + (" %d" % number if isinstance(number, int) else "")


def render(engine, size, samples, views):
    """Renders the test model from the given views; {"render", "pass", "detail"}."""
    global scene
    started = time.time()
    name = "%d view%s with %s" % (len(views), "" if len(views) == 1 else "s", engine)
    try:
        import bpy
        if scene is None:
            bpy.ops.mesh.primitive_monkey_add(location=(0, 0, 0))
            bpy.ops.object.light_add(type="SUN", location=(3, -3, 5))
            bpy.ops.object.camera_add(location=(0, -4, 1.5), rotation=(1.35, 0, 0))
            scene = bpy.context.scene
            scene.camera = bpy.context.active_object
        scene.render.engine = engine
        scene.render.resolution_x = scene.render.resolution_y = size
        scene.render.resolution_percentage = 100
        if engine == "CYCLES":
            scene.cycles.device = "CPU"
            scene.cycles.samples = samples
        files = []
        for view in views:
            x, y, z, rx, ry, rz = VIEWS[view]
            scene.camera.location = (x, y, z)
            scene.camera.rotation_euler = (rx, ry, rz)
            scene.render.filepath = os.path.join(job, "%s-%s-%d.png" % (engine.lower(), view, size))
            bpy.ops.render.render(write_still=True)
            files.append(scene.render.filepath)
        made = all(os.path.exists(f) and os.path.getsize(f) > (1000 if size >= 256 else 100) for f in files)
        return {"render": name, "pass": made, "detail": "%.1f s" % (time.time() - started)}
    except Exception as e:  # noqa: BLE001
        return {"render": name, "pass": False, "detail": "%s (%.1f s)" % (failure(e), time.time() - started)}


def eevee():
    import bpy
    engines = [e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items]
    found = [e for e in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT") if e in engines]
    return found[0] if found else None


if mode == "start":
    made = render("CYCLES", 32, 1, ["front"])
    with open(os.path.join(job, "started.txt"), "w") as f:
        f.write("rendered" if made["pass"] else "no image")
    raise SystemExit(0)
if mode == "runaway":
    with open(os.path.join(job, "runaway-started.txt"), "w") as f:
        f.write(str(os.getpid()))
    while True:
        pass

targets = json.load(open(os.path.join(job, "targets.json")))
home, outside, ppid = targets["home"], targets["outside"], targets["parent_pid"]
token = targets["token"].encode()
results = []
renders = []
info = []  # recorded, never scored


def check(name, action, control=True):
    """One check. opened: True when the action worked (in the box: a hole), False when it was refused, None when its
    target is absent. control False: no unboxed control can show the target (then a refusal stands on its own)."""
    try:
        detail = action() or "opened"
        results.append({"check": name, "opened": True, "detail": detail, "control": control})
    except FileNotFoundError:
        results.append({"check": name, "opened": None, "detail": "absent", "control": control})
    except Exception as e:  # noqa: BLE001 - any refusal counts
        results.append({"check": name, "opened": False, "detail": failure(e), "control": control})


# files: opened and closed at once
def open_folder(path):
    with os.scandir(path):
        pass


def open_file(path):
    os.close(os.open(path, os.O_RDONLY))


def write_file(path):
    with open(path, "w") as f:
        f.write("x")


check("open the home folder", lambda: open_folder(home))
check("open ~/.ssh", lambda: open_folder(os.path.join(home, ".ssh")))
check("open /home (other people's folders)", lambda: open_folder("/home"))
check("open the test's folder outside the job folder", lambda: open_folder(outside))
check("open the test's file outside the job folder", lambda: open_file(os.path.join(outside, "decoy.txt")))
check("write outside the job folder", lambda: write_file(os.path.join(outside, "written-from-the-box.txt")))
check("make a folder outside the job folder", lambda: os.mkdir(os.path.join(outside, "made-by-the-box")))
check("write in /tmp", lambda: write_file("/tmp/%s.txt" % targets["tmp_name"]))
check("write in /dev/shm", lambda: write_file("/dev/shm/%s" % targets["tmp_name"]))
check("write in Blender's folder", lambda: write_file(os.path.join(targets["blender_dir"], "written-from-the-box.txt")))
check("write in the scripts folder", lambda: write_file(os.path.join(targets["scripts"], "written-from-the-box.txt")))
if targets.get("runtime"):
    check("open the runtime folder (where Make's link socket lives)", lambda: open_folder(targets["runtime"]))
check("open /etc/hostname (outside the short /etc list)", lambda: open_file("/etc/hostname"))
# other processes' /proc entries: their command lines and environments can hold tokens (security 11-07, item d)
check("open the test program's environment (/proc/<its pid>/environ)", lambda: open_file("/proc/%d/environ" % ppid))
check("open the test program's command line (/proc/<its pid>/cmdline)", lambda: open_file("/proc/%d/cmdline" % ppid))
check("open process 1's command line (/proc/1/cmdline)", lambda: open_file("/proc/1/cmdline"))
check("open the test program's folder through /proc/<its pid>/cwd", lambda: open_folder("/proc/%d/cwd" % ppid))
check("reach ~/.ssh through /proc/self/root", lambda: open_folder("/proc/self/root" + os.path.join(home, ".ssh")))
check("reach ~/.ssh through /proc/<the test's pid>/root",
      lambda: open_folder("/proc/%d/root%s" % (ppid, os.path.join(home, ".ssh"))))


# network: socket() is refused for every family in the box (security 11-07, item a)
def listener(address):
    with socket.create_connection(tuple(address), timeout=5) as s:
        s.settimeout(5)
        return "connected, token " + ("ok" if s.recv(64).startswith(token) else "missing")


def closed_port(address):
    try:
        socket.create_connection(tuple(address), timeout=5).close()
        return "connected"
    except ConnectionRefusedError:
        return "answered refused"  # this machine's own network stack answered: the attempt got through


def unix_socket(address, expect_token):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(5)
        s.connect(address)
        if not expect_token:
            return "connected"  # the session bus: nothing sent, nothing read
        return "connected, token " + ("ok" if s.recv(64).startswith(token) else "missing")


def udp(address):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.sendto(token, tuple(address))
    return "sent"


def pair():
    a, b = socket.socketpair()
    a.close()
    b.close()
    return "made"


def netlink():
    socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, 0).close()
    return "opened"


def resolve(name):
    socket.getaddrinfo(name, 80)
    return "resolved"  # pass/fail only: no address kept


check("network: the test's own TCP listener on loopback", lambda: listener(targets["loopback"]))
check("network: a UDP datagram to the test's own loopback port", lambda: udp(targets["udp"]))
check("network: a closed TCP port on this machine's own address", lambda: closed_port(targets["own_closed"]))
check("network: the test's own Unix socket in the runtime folder (where Make's link lives)",
      lambda: unix_socket(targets["unix_path"], True))
check("network: the test's own abstract Unix socket", lambda: unix_socket("\0" + targets["unix_abstract"], True))
if targets.get("session_bus"):
    check("network: the session D-Bus (connect only; the keyring is behind it)",
          lambda: unix_socket(targets["session_bus"], False))
check("network: a Unix socket pair", pair)
check("network: a netlink socket (the kernel's network settings)", netlink)
check("network: this machine's own name (DNS)", lambda: resolve(socket.gethostname()))
check("network: a name under .invalid (DNS)", lambda: resolve("darkride-box-test.invalid"), control=False)

# programs, other processes, the terminal, memory
libc = ctypes.CDLL(None, use_errno=True)


def raw_syscall(*args):
    value = libc.syscall(*args)
    if value < 0:
        number = ctypes.get_errno()
        raise OSError(number, os.strerror(number))
    return value


def start_program():
    subprocess.run(["/usr/bin/true"], check=True, timeout=10)
    return "ran"


def fork():
    pid = os.fork()
    if pid == 0:
        os._exit(0)
    os.waitpid(pid, 0)
    return "forked"


def signal_zero(pid):
    os.kill(pid, 0)  # signal 0: only the permission check, nothing is delivered
    return "allowed"


def tkill_zero(tid):
    raw_syscall(200, tid, 0)  # tkill(tid, 0), x86_64
    return "allowed"


def io_uring():
    params = ctypes.create_string_buffer(120)  # struct io_uring_params
    os.close(raw_syscall(425, 4, params))  # io_uring_setup
    return "opened"


def tiocsti():
    import fcntl, termios
    fcntl.ioctl(0, termios.TIOCSTI, b"x")
    return "typed"


def terminal_fds():
    ttys = [fd for fd in (0, 1, 2) if os.isatty(fd)]
    if ttys:
        return "%d of 3 are terminals" % len(ttys)
    raise LookupError("none")  # none is a terminal: the good case, recorded as refused


def big_allocation():
    block = bytearray(5 << 30)
    del block
    return "allocated"


def user_namespace():
    raw_syscall(272, 0x10000000)  # unshare(CLONE_NEWUSER), x86_64
    return "made"


check("start another program", start_program)
check("fork", fork)
check("signal the test program (kill, signal 0)", lambda: signal_zero(ppid))
check("signal every process of this user (kill -1, signal 0)", lambda: signal_zero(-1))
check("signal the test program's thread (tkill, signal 0)", lambda: tkill_zero(ppid))
# the box lets a process signal itself (Blender's own threads, raise()): recorded, not scored
try:
    signal_zero(os.getpid())
    info.append({"check": "signal this process itself (the box allows it)", "detail": "allowed"})
except Exception as e:  # noqa: BLE001
    info.append({"check": "signal this process itself (the box allows it)", "detail": failure(e)})
check("io_uring (a way around system-call filters)", io_uring)
check("type into a terminal (TIOCSTI on standard input)", tiocsti, control=False)
check("standard input, output and error are terminals", terminal_fds, control=False)
check("open /dev/tty (a controlling terminal)", lambda: open_file("/dev/tty"), control=False)
check("a 5 GB allocation (the box's memory cap is 4 GB)", big_allocation)
leaked = [k for k, v in os.environ.items() if "ANTHROPIC" in k.upper() or "kit-test-env-key" in v]
results.append({"check": "no key in the environment", "opened": bool(leaked), "detail": "%d found" % len(leaked),
                "control": False})
shown = [k for k in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR", "SSH_AUTH_SOCK")
         if k in os.environ]
results.append({"check": "no display, session bus, runtime folder or SSH agent in the environment", "opened": bool(shown),
                "detail": "%d found" % len(shown), "control": False})


def write_results():
    with open(os.path.join(job, mode + "-results.json"), "w") as f:
        import bpy
        json.dump({"blender": bpy.app.version_string, "mode": mode, "results": results, "renders": renders,
                   "info": info}, f, indent=1)


if mode == "control":
    write_results()
    renders.append(render("CYCLES", 64, 4, ["front"]))
    write_results()  # EEVEE after Cycles: without a GPU it may take Blender down
    engine = eevee()
    renders.append(render(engine, 64, 0, ["front"]) if engine else {"render": "EEVEE", "pass": False, "detail": "no engine"})
    write_results()
    check("a new user namespace (unshare)", user_namespace)  # last: unboxed, it moves this process into one
    write_results()
    raise SystemExit(0)
check("a new user namespace (unshare)", user_namespace)

# probe: the verdicts against the control, then the renders, then the exec checks
try:
    control = {r["check"]: r for r in json.load(open(os.path.join(job, "control-results.json")))["results"]}
    control_renders = json.load(open(os.path.join(job, "control-results.json")))["renders"]
except Exception:  # noqa: BLE001
    control, control_renders = {}, []


def verdict(r):
    if r["opened"] is True:
        return "FAIL"
    if r["opened"] is None:
        return "UNKNOWN (absent)"
    if not r["control"]:
        return "PASS (refused; no control possible)"
    c = control.get(r["check"])
    if c and c["opened"] is True:
        return "PASS"
    return "UNKNOWN (the unboxed control didn't reach it either)"


def write_summary():
    lines = ["%s  %s  [%s]" % (verdict(r), r["check"], r["detail"]) for r in results]
    lines += ["INFO  %s  [%s]" % (i["check"], i["detail"]) for i in info]
    eevee_unboxed = any(r["render"].endswith(("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT")) and r["pass"] for r in control_renders)
    for r in renders:
        if r["pass"]:
            lines.append("PASS  render %s  [%s]" % (r["render"], r["detail"]))
        elif "EEVEE" in r["render"] and not eevee_unboxed:
            lines.append("INFO  render %s  [%s] (EEVEE fails unboxed too: no GPU here)" % (r["render"], r["detail"]))
        else:
            lines.append("FAIL  render %s  [%s]" % (r["render"], r["detail"]))
    failed = sum(1 for line in lines if line.startswith("FAIL"))
    unknown = sum(1 for line in lines if line.startswith("UNKNOWN"))
    lines.append("PROBE: PASS" if not failed and not unknown else
                 "PROBE: FAIL (%d failed, %d unknown)" % (failed, unknown) if failed else
                 "PROBE: INCOMPLETE (%d unknown)" % unknown)
    with open(os.path.join(job, "probe-summary.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")


def save():
    write_results()
    write_summary()


save()
renders.append(render("CYCLES", 256, 8, list(VIEWS)))
save()  # EEVEE last: without a GPU it may take Blender down
engine = eevee()
renders.append(render(engine, 256, 0, list(VIEWS)) if engine else {"render": "EEVEE", "pass": False, "detail": "no engine"})
save()


def exec_check(name, path, argv):
    """A program in Blender's place (no fork): the box refuses every exec after its own start of Blender. Results are
    saved first, since a working exec ends this process; the program then leaves its marker for the test to find."""
    save()
    try:
        os.execv(path, argv)
    except Exception as e:  # noqa: BLE001
        results.append({"check": name, "opened": False, "detail": failure(e), "control": False})


exec_check("run another program in Blender's place (exec /usr/bin/touch)", "/usr/bin/touch",
           ["touch", os.path.join(job, "exec-escaped-direct.txt")])
exec_check("run another program in Blender's place through the ELF loader", targets["loader"],
           [targets["loader"], "/usr/bin/touch", os.path.join(job, "exec-escaped-loader.txt")])
save()
