
# Declare dependencies
macro (setup_testcase_dependencies)
  # the action classes live in physics (which links Jolt publicly); the envelope and the entity list in entities, whose
  # headers reach avatars and the model readers (as tests/entity-content links)
  link_hifi_libraries(shared test-utils shaders octree gpu graphics fbx networking entities avatars model-networking physics)
  include_hifi_library_headers(hfm)
  include_hifi_library_headers(image)
  include_hifi_library_headers(ktx)
  include_hifi_library_headers(material-networking)
  include_hifi_library_headers(procedural)
  package_libraries_for_deployment()
endmacro ()

# ActionDataFormatTests: the bytes of an entity's actionData (its actions and constraints, on the network and in world
# files) as a Qt 5 build writes them, for the Qt 6 build to match
setup_hifi_testcase()
