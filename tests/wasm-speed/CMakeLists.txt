# The speed of JavaScript on the script engine, in one program: the JavaScript guest on WebAssembly (with and
# without the runtime's epoch checks) and, when its source is at hand, QuickJS-NG built natively, which separates
# the interpreter's cost from the sandbox's. No client and no domain are needed.

set(QUICKJS_NATIVE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/tools/wasm-js/build/quickjs-0.17.0" CACHE PATH
    "QuickJS-NG's source, for the native column of tests/wasm-speed (tools/wasm-js/build.sh unpacks and patches it)")

macro (setup_testcase_dependencies)
  link_hifi_libraries(shared wasm-scripting script-engine test-utils)
  if (EXISTS "${QUICKJS_NATIVE_SOURCE_DIR}/quickjs.c")
    set(QUICKJS_NATIVE_SOURCES dtoa.c libregexp.c libunicode.c quickjs.c)
    list(TRANSFORM QUICKJS_NATIVE_SOURCES PREPEND "${QUICKJS_NATIVE_SOURCE_DIR}/")
    target_sources(${TARGET_NAME} PRIVATE ${QUICKJS_NATIVE_SOURCES})
    set_source_files_properties(${QUICKJS_NATIVE_SOURCES} PROPERTIES COMPILE_DEFINITIONS _GNU_SOURCE COMPILE_OPTIONS "-w;-O2")
    target_include_directories(${TARGET_NAME} PRIVATE "${QUICKJS_NATIVE_SOURCE_DIR}")
    target_compile_definitions(${TARGET_NAME} PRIVATE HAVE_NATIVE_QUICKJS)
  endif ()
  package_libraries_for_deployment()
endmacro ()

setup_hifi_testcase()
