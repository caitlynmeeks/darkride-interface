#
#  prelude.py
#  libraries/make-link/resources/blender
#
#  Make's set-up, run by Blender before the script Claude wrote, inside the sandbox: an empty scene in metres, so every
#  make starts the same. From skunkworks' text-to-3d prototype (sandbox/prelude.py, 10-27).
#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#

import bpy

bpy.ops.wm.read_factory_settings(use_empty=True)
units = bpy.context.scene.unit_settings
units.system = "METRIC"
units.scale_length = 1.0
