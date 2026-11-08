#
#  darkride-make-watch: the parent of Make's Blender (src/main.cpp). It starts Blender's sandbox line as its own child
#  and kills it when the app stops it, when the app is gone, or at its wall-clock cap (security should-fix 4). Plain C++
#  with no Qt; built with the app and copied beside its program (Contents/MacOS on macOS), so the app starts it from
#  inside the signed app, never from a writable place. macOS and Linux (where it is the box as well: src/main.cpp).
#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#

set(TARGET_NAME darkride-make-watch)
project(${TARGET_NAME})
add_executable(${TARGET_NAME} src/main.cpp)
set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "Tools" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
