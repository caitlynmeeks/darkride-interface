
# Declare dependencies
macro (setup_testcase_dependencies)
  link_hifi_libraries(shared test-utils gpu shaders graphics render render-utils)
  package_libraries_for_deployment()
endmacro ()

# ShapePlumberTests: a shape key's pipeline, looked up the first time it's drawn, is the one the old expansion gave it
setup_hifi_testcase()
