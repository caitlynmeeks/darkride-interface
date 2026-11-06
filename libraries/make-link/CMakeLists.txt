#
#  make-link: Make's link to the builder's own Claude Code (the MCP server on a user-only socket, the tool list and
#  its checks) and the Blender sandbox. The app sets the tools' handlers.
#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#

set(TARGET_NAME make-link)
setup_hifi_library(Network)
