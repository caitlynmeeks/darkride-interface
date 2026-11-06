#
#  darkride-mcp: the helper the builder's Claude Code starts for Make (src/main.cpp). Plain C++ with no Qt, so it
#  starts in milliseconds. Built with the app and copied beside its program (Contents/MacOS on macOS), so the MCP
#  line Make writes names a file inside the signed app, never a writable place (security A5).
#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#

set(TARGET_NAME darkride-mcp)
project(${TARGET_NAME})
add_executable(${TARGET_NAME} src/main.cpp)
set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "Tools" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
