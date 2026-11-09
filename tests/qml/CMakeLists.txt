# Declare dependencies
macro (setup_testcase_dependencies)
  # link in the shared libraries
  link_hifi_libraries(shared networking gl qml)

  package_libraries_for_deployment()
endmacro ()

setup_hifi_testcase(Qml Quick)
