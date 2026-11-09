set(TARGET_NAME assignment-client)

setup_hifi_project(Core Gui Network Quick WebSockets)

# Fix up the rpath so macdeployqt works
if (APPLE)
  set_target_properties(${TARGET_NAME} PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
endif ()

setup_memory_debugger()

# link in the shared libraries
link_hifi_libraries(
  audio avatars octree gpu graphics shaders fbx hfm entities
  networking animation recording shared script-engine embedded-webserver
  controllers physics plugins image
  material-networking model-networking ktx shaders
  workload vehicles terrain
)
include_hifi_library_headers(procedural)

if (WIN32)
  package_libraries_for_deployment()
endif()

install_beside_console()

# the JavaScript guest of the WebAssembly script engine, for the entity script server's entity scripts
copy_javascript_guest("$<TARGET_FILE_DIR:${TARGET_NAME}>/resources")

# The default avatar's skeleton (its .fst and the mannequin's .fbx; an agent doesn't render, so no textures): an agent
# with no model of its own animates the mannequin that every viewer draws by default (ScriptableAvatar). About 3.6 MB
# in the server package. Nothing else writes resources/meshes, so a clean and an incremental build give the same tree.
# CMAKE_SOURCE_DIR, not PROJECT_SOURCE_DIR: setup_hifi_project makes this directory a project of its own.
set(DEFAULT_AVATAR_SOURCE "${CMAKE_SOURCE_DIR}/interface/resources/meshes")
set(DEFAULT_AVATAR_OUTPUT "$<TARGET_FILE_DIR:${TARGET_NAME}>/resources/meshes")
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${DEFAULT_AVATAR_OUTPUT}/mannequin"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DEFAULT_AVATAR_SOURCE}/defaultAvatar_full.fst"
          "${DEFAULT_AVATAR_OUTPUT}/defaultAvatar_full.fst"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DEFAULT_AVATAR_SOURCE}/mannequin/mannequin.fbx"
          "${DEFAULT_AVATAR_OUTPUT}/mannequin/mannequin.fbx")
