#!/usr/bin/env python3
#
# judder-report.py
# tests-manual/physics-baseline
#
# The summary of a run-judder.sh run: for each recorded phase, from the engine's frame trace (frame-trace-<phase>.csv,
# Test.dumpFrameTrace: the cam.* and ride.* columns per game-loop frame, physics.substeps, the substeps the frame ran,
# where the physics step's columns are in), how far the kart jumps on screen from one frame to the next (its centre projected through the
# frame's camera; pixels on a window 1080 high), how often the camera stood still while the kart moved (frames the
# script had not yet moved it), the kart's and the camera's steps per frame, the substeps per frame and the largest
# correction; from the render and present traces, the drawn frames and the presents that showed an old frame again.
#   tests-manual/physics-baseline/judder-report.py <results dir>
#
# Copyright 2026 Darkride.
#
# Distributed under the MIT License.

import csv
import glob
import json
import math
import os
import sys

HEIGHT_PX = 1080


def qinv_rotate(q, v):
    # the inverse of the unit quaternion q = (w, x, y, z) applied to v
    w, x, y, z = q[0], -q[1], -q[2], -q[3]
    vx, vy, vz = v
    # t = 2 * cross(q.xyz, v); v' = v + w * t + cross(q.xyz, t)
    tx = 2 * (y * vz - z * vy)
    ty = 2 * (z * vx - x * vz)
    tz = 2 * (x * vy - y * vx)
    return (vx + w * tx + (y * tz - z * ty), vy + w * ty + (z * tx - x * tz), vz + w * tz + (x * ty - y * tx))


def stats(values):
    if not values:
        return 0.0, 0.0, 0.0
    mean = sum(values) / len(values)
    rms = math.sqrt(sum(v * v for v in values) / len(values))
    return mean, rms, max(values)


def read_frame_trace(results):
    # every frame-trace-*.csv of the run (one per phase; a frame dumped twice counts once), the frames the avatar rode
    # something, oldest first: the camera and the ride as the frame showed them, the substeps the frame ran when the
    # physics step's column is in (None before it), the correction still blending in
    def number(cell):
        return float(cell) if cell not in (None, "") else None
    rows = {}
    for path in sorted(glob.glob(os.path.join(results, "frame-trace-*.csv"))):
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                if r.get("ride.x_m") in (None, "") or r.get("cam.x_m") in (None, ""):
                    continue
                frame = int(float(r["frame"]))
                substeps = number(r.get("physics.substeps"))
                rows[frame] = {"t": float(r["t_us"]) / 1000.0, "mode": int(number(r.get("cam.mode")) or 0),
                               "cam": tuple(number(r["cam.%s_m" % a]) for a in "xyz"),
                               "camq": tuple(number(r["cam.q%s" % a]) for a in "wxyz"),
                               "veh": tuple(number(r["ride.%s_m" % a]) for a in "xyz"),
                               "vehq": tuple(number(r["ride.q%s" % a]) for a in "wxyz"),
                               "substeps": int(substeps) if substeps is not None else None,
                               "correction": number(r.get("ride.correction_m")) or 0.0,
                               "ride_step": number(r.get("cam.ride_step_mm")), "step": number(r.get("cam.step_mm")),
                               "source": number(r.get("cam.source"))}
    return [rows[k] for k in sorted(rows)]


def main(results):
    with open(os.path.join(results, "judder.json")) as f:
        run = json.load(f)
    rows = read_frame_trace(results)
    fov = math.radians(run.get("fieldOfView") or 45.0)
    focal = (HEIGHT_PX / 2) / math.tan(fov / 2)
    print("camera %s, %s; display %.0f Hz, game loop %.0f Hz, render %.0f Hz, vsync %s; field of view %.1f degrees" % (
        run["camera"], "serverless" if run["serverless"] else "on a domain", run["displayRefreshRate"],
        run["gameLoopRate"], run["renderLoopRate"], run["vsync"], math.degrees(fov)))
    print("%-13s %6s %7s | %-27s | %-14s | %-17s | %-17s | %-9s %6s | %s" % (
        "phase", "m/s", "frames", "kart on screen, px/frame", "camera held", "kart mm/frame",
        "camera mm/frame", "substeps", "corr", "drawn/repeated"))
    for phase in run["phases"]:
        frames = [r for r in rows if phase["start"] <= r["t"] <= phase["end"]]
        if len(frames) < 3:
            print("%-13s too few frames in the trace (%d)" % (phase["name"], len(frames)))
            continue
        screen, kart_steps, cam_steps, held, distance_steps = [], [], [], 0, []
        substeps = {}
        previous = None
        for r in frames:
            rel = tuple(r["veh"][i] - r["cam"][i] for i in range(3))
            c = qinv_rotate(r["camq"], rel)   # the camera looks along its -z
            depth = max(0.01, -c[2])
            s = (focal * c[0] / depth, focal * c[1] / depth, math.sqrt(sum(x * x for x in rel)))
            if previous:
                p, ps = previous
                screen.append(math.hypot(s[0] - ps[0], s[1] - ps[1]))
                distance_steps.append(abs(s[2] - ps[2]) * 1000)
                kart = math.dist(r["veh"], p["veh"]) * 1000
                cam = math.dist(r["cam"], p["cam"]) * 1000
                kart_steps.append(kart)
                cam_steps.append(cam)
                if cam < 0.01 and kart > 1:
                    held += 1
                if r["substeps"] is not None:
                    n = r["substeps"]
                    substeps[n] = substeps.get(n, 0) + 1
            previous = (r, s)
        sm, sr, sx = stats(screen)
        km, kr, kx = stats(kart_steps)
        cm, cr, cx = stats(cam_steps)
        dm, dr, dx = stats(distance_steps)
        kart_dev = math.sqrt(sum((k - km) ** 2 for k in kart_steps) / len(kart_steps))
        cam_dev = math.sqrt(sum((c - cm) ** 2 for c in cam_steps) / len(cam_steps))
        corr = max(r["correction"] for r in frames)
        presents = phase.get("presentTrace") or []
        repeated = sum(1 for p in presents if not p[2])
        steps = " ".join("%d:%d" % (k, substeps[k]) for k in sorted(substeps)) or "-"
        print("%-13s %6.2f %7d | mean %4.1f rms %4.1f max %5.1f | %4d of %5d | %5.1f +-%5.1f %5.0f | %5.1f +-%5.1f %5.0f | %-9s %6.3f | %d/%d" % (
            phase["name"], phase["speed"], len(frames), sm, sr, sx, held, len(cam_steps), km, kart_dev, kx,
            cm, cam_dev, cx, steps, corr, len(phase.get("renderTrace") or []), repeated))
        print("%-13s camera-to-kart distance change per frame: mean %.1f mm, max %.1f mm; first frames' camera steps (mm): %s" % (
            "", dm, dx, " ".join("%.0f" % c for c in cam_steps[:24])))
        # the engine's own columns (item 11): the camera's step in the kart's frame (0 with the camera fixed in the
        # kart's frame, whatever the kart does), its world step and who placed it
        ride_steps = [r["ride_step"] for r in frames if r["ride_step"] is not None]
        engine_steps = [r["step"] for r in frames if r["step"] is not None]
        scripted = sum(1 for r in frames if r["source"])
        if ride_steps and engine_steps:
            rm, rr, rx = stats(ride_steps)
            print("%-13s engine: camera step in the kart's frame (cam.ride_step_mm) mean %.2f max %.2f mm of %d; world step (cam.step_mm) min %.1f max %.1f mm; script-set camera (cam.source) %d of %d frames" % (
                "", rm, rx, len(ride_steps), min(engine_steps), max(engine_steps), scripted, len(frames)))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
