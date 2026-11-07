#
#  make-link: Make's link to the builder's own Claude Code (the MCP server on a user-only socket, the tool list and
#  its checks) and the Blender sandbox. The app sets the tools' handlers.
#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#

set(TARGET_NAME make-link)
setup_hifi_library(Network Gui)

if (APPLE)
  # MakeKey: the builder's key in the login keychain; MakeBlender: the memory check (libproc is in libSystem)
  target_link_libraries(${TARGET_NAME} "-framework Security" "-framework CoreFoundation")
endif ()
