# Declare dependencies
macro (setup_testcase_dependencies)
  # link in the shared libraries (VerifiedSandboxTests makes WebAssembly engines: the edit filter tests' list)
  link_hifi_libraries(shared test-utils shaders octree gpu graphics fbx networking entities avatars audio animation model-networking controllers pointers wasm-scripting script-engine)
  include_hifi_library_headers(hfm)
  include_hifi_library_headers(image)
  include_hifi_library_headers(ktx)
  include_hifi_library_headers(material-networking)
  include_hifi_library_headers(procedural)
  include_hifi_library_headers(platform)
  package_libraries_for_deployment()
endmacro ()

setup_hifi_testcase(Network WebSockets)
