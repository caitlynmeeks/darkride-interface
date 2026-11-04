
# Declare dependencies
macro (SETUP_TESTCASE_DEPENDENCIES)
  # the physics library links Jolt publicly, so the tests get it from there
  link_hifi_libraries(shared test-utils physics gpu graphics)
  package_libraries_for_deployment()
endmacro ()

setup_hifi_testcase()
