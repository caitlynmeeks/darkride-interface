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
if (WIN32)
  # MakeFirewall: the read of the installer's rules for Blender's box (INetFwPolicy2, COM) and the box's SID;
  # MakeKey: the builder's key in Credential Manager; MakeBox: the AppContainer profile and its grants (userenv,
  # advapi32), the self-test's control connection (ws2_32) and Blender's version from its file (version); MakeClaude:
  # the known folders for the key route's environment (shell32)
  target_link_libraries(${TARGET_NAME} ole32 oleaut32 userenv advapi32 ws2_32 version shell32)
endif ()
