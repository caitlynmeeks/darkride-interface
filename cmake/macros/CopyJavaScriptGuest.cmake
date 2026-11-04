#
#  CopyJavaScriptGuest.cmake
#  cmake/macros
#
#  The JavaScript guest, darkride-js.wasm (QuickJS-NG compiled to WebAssembly with the shim that gives a script
#  the engine's objects; see tools/wasm-js), copied beside a target's resources after the target builds. The
#  engine looks for it at resources/wasm/darkride-js.wasm (Contents/Resources/wasm on macOS), or where
#  $DARKRIDE_JS_WASM points.
#
#  The module is taken from, in this order: $ENV{DARKRIDE_JS_WASM}; tools/wasm-js/build/darkride-js.wasm (a local
#  build.sh; not in a PRODUCTION or PR build); the published module, checked by its SHA-256, from
#  ../downloads/darkride-js-<version>.wasm beside the checkout or downloaded once from the CDN's deps/ into the build
#  tree. Every script engine is the WebAssembly one, so a build without the guest could run no JavaScript:
#  configuring stops here instead.
#
#  The version is QuickJS-NG's, then a revision after the dash that goes up whenever the guest's imports or
#  exports change, so that a published module only reaches the builds whose host matches it, and whenever a
#  release needs a changed guest, so that every build of it carries the same one. Revision 2 asks the host
#  whether to stop (darkride.interrupted), which the time limit per script needs; a host from before it refuses
#  that import. Revision 3 names the script a request or socket is made for (the network rule for a script's
#  own requests, ScriptNetworkPolicy); its imports and exports are revision 2's, so either host runs either.
#  Revision 4 keeps the engine's request and socket makers to the shim and names the entity script asking for a
#  file to include or require; imports and exports unchanged.
#  Revision 5 names a global a world's entity scripts don't get (the script API policy's stand-in) in the error a
#  call on it raises; imports and exports unchanged.
#  Revision 6 exports console_eval, a line typed into the scripting console evaluated in the script's global scope;
#  imports unchanged. A host before it never calls it; this host tells the console when a guest lacks it.
#
#  Distributed under the Apache License, Version 2.0.
#  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
#
macro(COPY_JAVASCRIPT_GUEST DESTINATION_DIR)
    set(JAVASCRIPT_GUEST_VERSION "0.17.0-6")
    set(JAVASCRIPT_GUEST_SHA256 "b1ab3f7993850f3c3b28ba67cf732bfaddfaa4e4198e195c6bff6b12d23d7ad3")
    set(JAVASCRIPT_GUEST_URL "https://cdn.darkride.ai/deps/darkride-js-${JAVASCRIPT_GUEST_VERSION}.wasm")
    set(JAVASCRIPT_GUEST_NAME "darkride-js-${JAVASCRIPT_GUEST_VERSION}.wasm")
    set(JAVASCRIPT_GUEST "")
    # a guest of one's own: named by $DARKRIDE_JS_WASM, or built here with tools/wasm-js/build.sh (not in a release build,
    # which carries the published module only)
    if (NOT "$ENV{DARKRIDE_JS_WASM}" STREQUAL "" AND EXISTS "$ENV{DARKRIDE_JS_WASM}")
        set(JAVASCRIPT_GUEST "$ENV{DARKRIDE_JS_WASM}")
    elseif (NOT "${RELEASE_TYPE}" STREQUAL "PRODUCTION" AND NOT "${RELEASE_TYPE}" STREQUAL "PR" AND
            EXISTS "${CMAKE_SOURCE_DIR}/tools/wasm-js/build/darkride-js.wasm")
        set(JAVASCRIPT_GUEST "${CMAKE_SOURCE_DIR}/tools/wasm-js/build/darkride-js.wasm")
        message(STATUS "JavaScript guest: this checkout's own build (tools/wasm-js/build), not the published ${JAVASCRIPT_GUEST_NAME}")
    endif()
    # the published module, by its hash: from ../downloads beside the checkout, else downloaded once into the build tree
    if ("${JAVASCRIPT_GUEST}" STREQUAL "")
        foreach(JAVASCRIPT_GUEST_CANDIDATE
                "${CMAKE_SOURCE_DIR}/../downloads/${JAVASCRIPT_GUEST_NAME}"
                "${CMAKE_BINARY_DIR}/downloads/${JAVASCRIPT_GUEST_NAME}")
            if ("${JAVASCRIPT_GUEST}" STREQUAL "" AND EXISTS "${JAVASCRIPT_GUEST_CANDIDATE}")
                file(SHA256 "${JAVASCRIPT_GUEST_CANDIDATE}" JAVASCRIPT_GUEST_CANDIDATE_SHA256)
                if ("${JAVASCRIPT_GUEST_CANDIDATE_SHA256}" STREQUAL "${JAVASCRIPT_GUEST_SHA256}")
                    set(JAVASCRIPT_GUEST "${JAVASCRIPT_GUEST_CANDIDATE}")
                else()
                    message(WARNING "${JAVASCRIPT_GUEST_CANDIDATE} is not the published ${JAVASCRIPT_GUEST_NAME} (its SHA-256 differs): not used")
                endif()
            endif()
        endforeach()
    endif()
    if ("${JAVASCRIPT_GUEST}" STREQUAL "")
        set(JAVASCRIPT_GUEST_DOWNLOAD "${CMAKE_BINARY_DIR}/downloads/${JAVASCRIPT_GUEST_NAME}")
        message(STATUS "JavaScript guest: downloading ${JAVASCRIPT_GUEST_URL}")
        file(DOWNLOAD "${JAVASCRIPT_GUEST_URL}" "${JAVASCRIPT_GUEST_DOWNLOAD}.part" TLS_VERIFY ON
             STATUS JAVASCRIPT_GUEST_STATUS)
        list(GET JAVASCRIPT_GUEST_STATUS 0 JAVASCRIPT_GUEST_STATUS_CODE)
        if (JAVASCRIPT_GUEST_STATUS_CODE EQUAL 0)
            file(SHA256 "${JAVASCRIPT_GUEST_DOWNLOAD}.part" JAVASCRIPT_GUEST_DOWNLOAD_SHA256)
            if (NOT "${JAVASCRIPT_GUEST_DOWNLOAD_SHA256}" STREQUAL "${JAVASCRIPT_GUEST_SHA256}")
                set(JAVASCRIPT_GUEST_STATUS "its SHA-256 is ${JAVASCRIPT_GUEST_DOWNLOAD_SHA256}, not the published one")
                set(JAVASCRIPT_GUEST_STATUS_CODE 1)
            endif()
        endif()
        if (JAVASCRIPT_GUEST_STATUS_CODE EQUAL 0)
            file(RENAME "${JAVASCRIPT_GUEST_DOWNLOAD}.part" "${JAVASCRIPT_GUEST_DOWNLOAD}")
            set(JAVASCRIPT_GUEST "${JAVASCRIPT_GUEST_DOWNLOAD}")
        else()
            file(REMOVE "${JAVASCRIPT_GUEST_DOWNLOAD}.part")
            message(FATAL_ERROR "No JavaScript guest module, and no script can run without one: the download of "
                "${JAVASCRIPT_GUEST_URL} failed (${JAVASCRIPT_GUEST_STATUS}). Put ${JAVASCRIPT_GUEST_NAME} in "
                "${CMAKE_SOURCE_DIR}/../downloads, set DARKRIDE_JS_WASM, or build one with tools/wasm-js/build.sh.")
        endif()
    endif()
    message(STATUS "JavaScript guest from ${JAVASCRIPT_GUEST}")
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DESTINATION_DIR}/wasm"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${JAVASCRIPT_GUEST}" "${DESTINATION_DIR}/wasm/darkride-js.wasm")
endmacro()
