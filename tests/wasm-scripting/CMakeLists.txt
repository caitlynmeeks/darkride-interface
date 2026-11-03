
# Declare dependencies
macro (setup_testcase_dependencies)
  link_hifi_libraries(shared wasm-scripting test-utils)
  package_libraries_for_deployment()
endmacro ()

# Script: HostInterfaceTests checks that a QScriptValue parameter is refused, until the host's QtScript forms go
setup_hifi_testcase(Script)
