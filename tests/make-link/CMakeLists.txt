# Make's link: the tool list's checks, the user-only socket, MCP over it, and the darkride-mcp helper
macro (setup_testcase_dependencies)
  link_hifi_libraries(make-link)
  package_libraries_for_deployment()
endmacro ()

setup_hifi_testcase(Network Gui)

if (TARGET darkride-mcp AND TARGET make-link-MakeLinkTests)
  add_dependencies(make-link-MakeLinkTests darkride-mcp)
  target_compile_definitions(make-link-MakeLinkTests PRIVATE DARKRIDE_MCP_PATH="$<TARGET_FILE:darkride-mcp>")
endif()

if (TARGET darkride-make-watch AND TARGET make-link-MakeBlenderTests)
  add_dependencies(make-link-MakeBlenderTests darkride-make-watch)
  target_compile_definitions(make-link-MakeBlenderTests PRIVATE DARKRIDE_MAKE_WATCH_PATH="$<TARGET_FILE:darkride-make-watch>")
endif()
