#
#  finish.py
#  libraries/make-link/resources/blender
#
#  Make's fixed export, run by Blender after the script Claude wrote (MakeBlender: prelude.py, the script, this, then
#  views.py), inside the sandbox. Curves and text become meshes, hidden helpers and cameras go, and the model is moved
#  to stand on the ground at the centre. report.json says what was made: each part's name, size, triangles and
#  materials, and what glTF can't carry. Then model.glb, with modifiers applied. Everything goes into $MAKE_OUT.
#
#  MAKE_KIND=room (a room with baked light): the lights stay for the bake. Every object gets a second UV set
#  ("Lightmap", one shared atlas), Cycles bakes the light falling on each surface (direct and bounced, without the
#  surface's own colour: colour = lightmap x albedo) into lightmap.exr (half float, ZIP), then the lights go and the
#  model is exported with both UV sets.
#
#  From skunkworks' text-to-3d prototype (sandbox/finish.py, 10-27).
#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#

import bpy, json, math, os, random, time
from mathutils import Vector

scene = bpy.context.scene
report = {"warnings": []}
OUT = os.environ["MAKE_OUT"]
ROOM = os.environ.get("MAKE_KIND") == "room"
FINAL = False  # the quick bake: the final one takes minutes


def hidden(o):
    return o.hide_render or o.hide_viewport or not o.visible_get()


# everything visible becomes a plain mesh with its modifiers applied (curves, text and surfaces too), so it exports
# as it looks; then hidden helpers (a boolean's cutter) can go without undoing their cut
convert = [o for o in scene.objects if o.type in {"MESH", "CURVE", "FONT", "SURFACE", "META"} and not hidden(o)]
if convert:
    for o in scene.objects:
        o.select_set(o in convert)
    bpy.context.view_layer.objects.active = convert[0]
    bpy.ops.object.convert(target="MESH")

lights = [o for o in scene.objects if o.type == "LIGHT" and not hidden(o)] if ROOM else []
dropped = [o for o in scene.objects if (o.type in {"LIGHT", "CAMERA", "SPEAKER", "LIGHT_PROBE"} and o not in lights) or
           (o.type not in {"EMPTY", "LIGHT"} and hidden(o))]
report["dropped"] = sorted({o.type.lower() + (" (hidden)" if hidden(o) else "") for o in dropped})
for o in dropped:
    bpy.data.objects.remove(o, do_unlink=True)

meshes = [o for o in scene.objects if o.type == "MESH"]
if not meshes:
    raise SystemExit("finish: the script made no visible mesh objects")

depsgraph = bpy.context.evaluated_depsgraph_get()


def world_box(objs):
    lo, hi = Vector((1e9, 1e9, 1e9)), Vector((-1e9, -1e9, -1e9))
    for o in objs:
        ev = o.evaluated_get(depsgraph)
        for c in ev.bound_box:
            p = ev.matrix_world @ Vector(c)
            lo = Vector(map(min, lo, p))
            hi = Vector(map(max, hi, p))
    return lo, hi


# stand it on the ground, centred: move the top-level objects (children follow, lights too)
lo, hi = world_box(meshes)
shift = Vector((-(lo.x + hi.x) / 2, -(lo.y + hi.y) / 2, -lo.z))
if shift.length > 0.001:
    for o in scene.objects:
        if o.parent is None:
            o.location += shift
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    report["moved"] = [round(v, 3) for v in shift]
lo, hi = world_box(meshes)
report["size_m"] = {"width_x": round(hi.x - lo.x, 3), "depth_y": round(hi.y - lo.y, 3), "height_z": round(hi.z - lo.z, 3)}

parts, total = [], 0
for o in sorted(meshes, key=lambda o: o.name):
    ev = o.evaluated_get(depsgraph)
    mesh = ev.to_mesh()
    tris = sum(len(p.vertices) - 2 for p in mesh.polygons)
    ev.to_mesh_clear()
    total += tris
    plo, phi = world_box([o])
    parts.append({"name": o.name, "triangles": tris, "size": [round(v, 3) for v in (phi - plo)],
                  "centre": [round(v, 3) for v in (plo + phi) / 2],
                  "materials": [s.material.name for s in o.material_slots if s.material]})
    if not o.material_slots or not any(s.material for s in o.material_slots):
        report["warnings"].append(o.name + " has no material (it will be plain white)")
report["parts"] = parts
report["triangles"] = total

# what glTF can't carry: a Principled BSDF input fed by anything but an image texture is lost in the export
for m in bpy.data.materials:
    if not m.users or not m.use_nodes:
        continue
    out = next((n for n in m.node_tree.nodes if n.type == "OUTPUT_MATERIAL" and n.is_active_output), None)
    surface = out.inputs["Surface"].links[0].from_node if out and out.inputs["Surface"].links else None
    if surface is None or surface.type != "BSDF_PRINCIPLED":
        report["warnings"].append("material " + m.name + " isn't a plain Principled BSDF: glTF keeps only its colour")
        continue
    for name in ("Base Color", "Roughness", "Metallic", "Emission Color", "Alpha"):
        sock = surface.inputs.get(name)
        if sock and sock.links and sock.links[0].from_node.type not in {"TEX_IMAGE", "NORMAL_MAP"}:
            report["warnings"].append("material " + m.name + ": " + name + " comes from a " +
                                      sock.links[0].from_node.bl_idname + " node, which glTF drops (use a plain value)")


def weighted(samples, q):
    """the q quantile of (value, weight) pairs"""
    if not samples:
        return 0.0
    ordered = sorted(samples)
    total, running = sum(w for _, w in ordered), 0.0
    for value, w in ordered:
        running += w
        if running >= q * total:
            return value
    return ordered[-1][0]


def select_only(objs):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]


def bake_lightmap():
    import numpy
    size = 1024 if FINAL else 512
    t0 = time.time()
    report["lights"] = [{"name": o.name, "type": o.data.type, "power_w": round(o.data.energy, 1),
                         "colour": [round(c, 2) for c in o.data.color],
                         "at": [round(v, 2) for v in o.matrix_world.translation]} for o in lights]
    # each object its own mesh (a mesh shared by two objects would share one patch of the atlas), and a material
    for o in meshes:
        if o.data.users > 1:
            o.data = o.data.copy()
        if not any(s.material for s in o.material_slots):
            plain = bpy.data.materials.get("plain") or bpy.data.materials.new("plain")
            plain.use_nodes = True
            o.data.materials.append(plain)
    # the lightmap UVs: a second set, every object's islands packed into one atlas at the same texel density
    for o in meshes:
        uv = o.data.uv_layers
        if len(uv) == 0:
            uv.new(name="UVMap")
        while len(uv) > 1:
            uv.remove(uv[1])
        uv[0].active_render = True
        uv.active = uv.new(name="Lightmap")
    select_only(meshes)
    margin = 4.0 / size
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(66), island_margin=margin, area_weight=0.0, scale_to_bounds=False)
    bpy.ops.uv.select_all(action="SELECT")
    bpy.ops.uv.average_islands_scale()
    bpy.ops.uv.pack_islands(rotate=True, margin=margin)
    bpy.ops.object.mode_set(mode="OBJECT")
    # the bake target: an unlinked, active image node in every material
    image = bpy.data.images.new("lightmap", size, size, alpha=False, float_buffer=True)
    image.colorspace_settings.name = "Linear Rec.709"
    materials = {s.material for o in meshes for s in o.material_slots if s.material}
    for m in materials:
        m.use_nodes = True
        node = m.node_tree.nodes.new("ShaderNodeTexImage")
        node.name = "__lightmap"
        node.image = image
        m.node_tree.nodes.active = node
    # no sky of its own: a dim daylight one, so the outside walls aren't black under the world's sun (Darkride lights
    # them with the sun, and takes the rest from the lightmap) and a little daylight comes in at the door
    if scene.world is None:
        sky = bpy.data.worlds.new("daylight")
        sky.use_nodes = True
        background = sky.node_tree.nodes["Background"]
        background.inputs["Color"].default_value = (0.55, 0.65, 0.8, 1)
        background.inputs["Strength"].default_value = 0.6
        scene.world = sky
        report["sky"] = "none given: a dim daylight sky was used"
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 384 if FINAL else 64
    scene.cycles.max_bounces = 4
    scene.cycles.diffuse_bounces = 4
    scene.cycles.sample_clamp_indirect = 3.0 if FINAL else 1.5   # bounced light's rare bright samples (fireflies) clipped
    scene.render.bake.margin = 4
    scene.render.bake.margin_type = "EXTEND"
    select_only(meshes)
    bpy.ops.object.bake(type="DIFFUSE", pass_filter={"DIRECT", "INDIRECT"}, margin=4, use_clear=True)
    # half float EXR, linear
    settings = scene.render.image_settings
    settings.file_format = "OPEN_EXR"
    settings.color_depth = "16"
    settings.exr_codec = "ZIP"
    settings.color_mode = "RGB"
    scene.view_settings.view_transform = "Standard"
    image.save_render(os.path.join(OUT, "lightmap.exr"), scene=scene)
    # how bright it came out, inside: the light read from the lightmap at points spread over every surface (about one
    # per 0.02 m2; 1.0 shows a surface at its own full colour), for the points inside the room's box (its outside
    # walls are left out)
    px = numpy.empty(size * size * 4, dtype=numpy.float32)
    image.pixels.foreach_get(px)
    px = px.reshape(size, size, 4)
    weights = numpy.array([0.2126, 0.7152, 0.0722], dtype=numpy.float32)
    inner_lo, inner_hi = lo + Vector((0.03, 0.03, 0.03)), hi - Vector((0.03, 0.03, 0.03))
    rng = random.Random(1)
    inside, everything, by_object = [], [], {}
    for o in meshes:
        mesh, mw = o.data, o.matrix_world
        uv = mesh.uv_layers["Lightmap"].data
        scale = abs(mw.to_3x3().determinant()) ** (2 / 3)
        mesh.calc_loop_triangles()
        own = []
        for tri in mesh.loop_triangles:
            area = tri.area * scale
            n = min(64, max(1, int(area / 0.02)))
            for _ in range(n):
                r1, r2 = rng.random(), rng.random()
                if r1 + r2 > 1:
                    r1, r2 = 1 - r1, 1 - r2
                w = (1 - r1 - r2, r1, r2)
                u = sum(w[k] * uv[tri.loops[k]].uv[0] for k in range(3))
                v = sum(w[k] * uv[tri.loops[k]].uv[1] for k in range(3))
                at = mw @ sum((w[k] * mesh.vertices[tri.vertices[k]].co for k in range(3)), Vector())
                light = float(px[min(size - 1, max(0, int(v * size))), min(size - 1, max(0, int(u * size))), :3] @ weights)
                if light < 1e-4:   # no light at all: a face hidden inside or under something
                    continue
                sample = (light, area / n)
                everything.append(sample)
                if all(inner_lo[k] < at[k] < inner_hi[k] for k in range(3)):
                    inside.append(sample)
                    own.append(sample)
        if own:
            by_object[o.name] = (weighted(own, 0.5), sum(a for _, a in own))

    samples = inside or everything
    report["lightmap"] = {"size": size, "samples": scene.cycles.samples, "bake_seconds": round(time.time() - t0, 1),
                          "light_on_surfaces" + ("_inside" if inside else ""): {
                              "darkest_10pc": round(weighted(samples, 0.1), 3), "median": round(weighted(samples, 0.5), 3),
                              "brightest_5pc": round(weighted(samples, 0.95), 3)},
                          "median_by_object": {name: round(m, 3) for name, (m, _) in
                                               sorted(by_object.items(), key=lambda kv: -kv[1][1])[:12]}}
    for m in materials:
        m.node_tree.nodes.remove(m.node_tree.nodes["__lightmap"])
    for o in lights:
        bpy.data.objects.remove(o, do_unlink=True)


if ROOM:
    if not lights and not any(s.material and s.material.use_nodes and any(
            n.type == "BSDF_PRINCIPLED" and n.inputs["Emission Strength"].default_value > 0 for n in s.material.node_tree.nodes)
            for o in meshes for s in o.material_slots):
        report["warnings"].append("the room has no lights and nothing glows: the bake will be dark")
    bake_lightmap()

options = {"filepath": os.path.join(OUT, "model.glb"), "export_format": "GLB", "export_apply": True, "export_yup": True,
           "export_cameras": False, "export_lights": False, "export_extras": False, "export_animations": False,
           "export_texcoords": True}
known = bpy.ops.export_scene.gltf.get_rna_type().properties.keys()
bpy.ops.export_scene.gltf(**{k: v for k, v in options.items() if k in known})

with open(os.path.join(OUT, "report.json"), "w") as f:
    json.dump(report, f, indent=1)
print("finish: exported", len(meshes), "objects,", total, "triangles")
