#
#  views.py
#  libraries/make-link/resources/blender
#
#  Make's four views of a model, run by Blender after finish.py has exported it, inside the sandbox: the model on a
#  ground with a 1 m grid, under a sun and a sky, with a grey 1.75 m figure beside it for scale. The views are the front
#  (from -Y in Blender), the right side, from behind at three-quarters, and from above, each a square PNG in $MAKE_OUT
#  (view-front.png, view-right.png, view-back.png, view-top.png). The app puts them together into one picture for
#  Claude. Nothing here is exported: the ground, the figure, the lights and the cameras are for the pictures only.
#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#

import bpy, math, os
from mathutils import Vector

OUT = os.environ["MAKE_OUT"]
SIZE = int(os.environ.get("MAKE_VIEW_SIZE", "512"))
SAMPLES = int(os.environ.get("MAKE_VIEW_SAMPLES", "16"))
FIGURE_HEIGHT = 1.75

scene = bpy.context.scene
meshes = [o for o in scene.objects if o.type == "MESH"]
depsgraph = bpy.context.evaluated_depsgraph_get()
lo, hi = Vector((1e9, 1e9, 1e9)), Vector((-1e9, -1e9, -1e9))
for o in meshes:
    ev = o.evaluated_get(depsgraph)
    for c in ev.bound_box:
        p = ev.matrix_world @ Vector(c)
        lo = Vector(map(min, lo, p))
        hi = Vector(map(max, hi, p))
if not meshes:
    raise SystemExit("views: nothing to show")


def plain(name, colour, roughness=0.8):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    bsdf = material.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (*colour, 1)
    bsdf.inputs["Roughness"].default_value = roughness
    return material


# the figure for scale, standing beside the model on its right (+X)
figure_x = hi.x + 0.6
bpy.ops.mesh.primitive_cylinder_add(radius=0.2, depth=FIGURE_HEIGHT - 0.3, location=(figure_x, (lo.y + hi.y) / 2, (FIGURE_HEIGHT - 0.3) / 2))
body = bpy.context.active_object
bpy.ops.mesh.primitive_uv_sphere_add(radius=0.15, location=(figure_x, (lo.y + hi.y) / 2, FIGURE_HEIGHT - 0.15))
head = bpy.context.active_object
grey = plain("figure", (0.25, 0.25, 0.25))
for part in (body, head):
    part.data.materials.append(grey)

# what the views frame: the model and the figure
lo.x = min(lo.x, figure_x - 0.2)
hi.x = max(hi.x, figure_x + 0.2)
hi.z = max(hi.z, FIGURE_HEIGHT)
centre = (lo + hi) / 2
radius = max((hi - lo).length / 2, 0.5)

# the ground: 1 m squares
ground_size = max(12.0, math.ceil(radius * 8))
bpy.ops.mesh.primitive_plane_add(size=ground_size, location=(centre.x, centre.y, 0))
ground = bpy.context.active_object
grid = bpy.data.materials.new("ground")
grid.use_nodes = True
nodes = grid.node_tree.nodes
checker = nodes.new("ShaderNodeTexChecker")
checker.inputs["Scale"].default_value = ground_size
checker.inputs["Color1"].default_value = (0.30, 0.30, 0.30, 1)
checker.inputs["Color2"].default_value = (0.38, 0.38, 0.38, 1)
grid.node_tree.links.new(checker.outputs["Color"], nodes.get("Principled BSDF").inputs["Base Color"])
ground.data.materials.append(grid)

# the sun and the sky
bpy.ops.object.light_add(type="SUN", rotation=(math.radians(50), 0, math.radians(30)))
bpy.context.active_object.data.energy = 3.0
sky = bpy.data.worlds.new("sky")
sky.use_nodes = True
background = sky.node_tree.nodes["Background"]
background.inputs["Color"].default_value = (0.45, 0.55, 0.7, 1)
background.inputs["Strength"].default_value = 0.8
scene.world = sky

scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = SAMPLES
scene.cycles.use_denoising = True
scene.render.resolution_x = SIZE
scene.render.resolution_y = SIZE
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = "PNG"
scene.render.image_settings.color_mode = "RGB"
scene.view_settings.view_transform = "Standard"

camera_data = bpy.data.cameras.new("view")
camera_data.lens = 50
camera = bpy.data.objects.new("view", camera_data)
scene.collection.objects.link(camera)
scene.camera = camera
fov = camera_data.angle
distance = radius / math.sin(fov / 2) * 1.05

VIEWS = [
    ("front", Vector((0, -1, 0.25))),
    ("right", Vector((1, 0, 0.25))),
    ("back", Vector((-0.7, 0.7, 0.35))),
    ("top", Vector((0, -0.08, 1))),
]
for name, direction in VIEWS:
    direction.normalize()
    camera.location = centre + direction * distance
    camera.rotation_euler = (centre - camera.location).to_track_quat("-Z", "Y").to_euler()
    camera_data.clip_end = distance * 4
    scene.render.filepath = os.path.join(OUT, "view-" + name + ".png")
    bpy.ops.render.render(write_still=True)
print("views: rendered", len(VIEWS))
