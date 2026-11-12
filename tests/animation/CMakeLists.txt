# Declare dependencies
macro (setup_testcase_dependencies)
  # link in the shared libraries
  link_hifi_libraries(shared animation gpu fbx hfm graphics networking test-utils image task model-baker auto-rigging unity-import)

  package_libraries_for_deployment()
endmacro ()

setup_hifi_testcase()
