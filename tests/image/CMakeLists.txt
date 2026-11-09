
# Declare dependencies
macro (setup_testcase_dependencies)
  link_hifi_libraries(shared ktx gpu image)
  target_openexr()
  package_libraries_for_deployment()
endmacro ()

# ImageTests: pictures and EXR files that are not what their headers say are refused, never a crash
setup_hifi_testcase()
