
# Declare dependencies
macro (setup_testcase_dependencies)
  link_hifi_libraries(shared test-utils shaders octree gpu graphics fbx networking entities avatars audio animation model-networking wasm-scripting script-engine)
  include_hifi_library_headers(hfm)
  include_hifi_library_headers(image)
  include_hifi_library_headers(ktx)
  include_hifi_library_headers(material-networking)
  include_hifi_library_headers(procedural)
  package_libraries_for_deployment()
endmacro ()

# EntityRightsTests: what the entity server lets a node change, delete and add, by its rights
setup_hifi_testcase(Network)
