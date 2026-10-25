#
#  TargetWasmtime.cmake
#  cmake/macros
#
#  wasmtime (Apache-2.0 with the LLVM exception, https://wasmtime.dev), the WebAssembly runtime, taken as the
#  prebuilt C API of a pinned release: a static library (on Windows its DLL) and its headers, one archive per
#  platform. It is deliberately not a vcpkg port: adding a port changes the vcpkg hash and rebuilds every dependency
#  beside the shared install the other checkouts use.
#
#  The archive is taken from $TIVOLI_DOWNLOADS/<archive> or ../downloads/<archive> beside this checkout when
#  either exists, and from GitHub otherwise. Its SHA-256 is checked in every case.
#
#  Distributed under the Apache License, Version 2.0.
#  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
#
macro(TARGET_WASMTIME)
    if (NOT TARGET wasmtime::wasmtime)
        include(FetchContent)
        set(WASMTIME_VERSION "49.0.3")
        set(WASMTIME_EXTENSION "tar.xz")
        if (APPLE)
            if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
                set(WASMTIME_PLATFORM "aarch64-macos")
                set(WASMTIME_SHA256 "0c853eb1715b3730304f926d111d4b4a3605b961c5693753bfeeb6b19332c41b")
            else()
                set(WASMTIME_PLATFORM "x86_64-macos")
                set(WASMTIME_SHA256 "6d8749e2e0ea1360832c6b0fac76804e74f7ad4ff20ea18cc0f66f9195991313")
            endif()
        elseif (WIN32)
            set(WASMTIME_PLATFORM "x86_64-windows")
            set(WASMTIME_EXTENSION "zip")
            set(WASMTIME_SHA256 "f8dabbc1df2358f15effbd6e50c940c03df3836e13cbf4f09cdf6369057aed01")
        elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
            if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
                set(WASMTIME_PLATFORM "aarch64-linux")
                set(WASMTIME_SHA256 "fb9c1a101c092cd3b4536ddb4c68ae31ad70d78bbdbc408d0e007f05a2fdd409")
            else()
                set(WASMTIME_PLATFORM "x86_64-linux")
                set(WASMTIME_SHA256 "49c9641ac1f6158ed90404115315b36a9ae68338acda16fcf00f0819622ccd97")
            endif()
        else()
            message(FATAL_ERROR "wasmtime: no prebuilt C API for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
        endif()
        set(WASMTIME_ARCHIVE "wasmtime-v${WASMTIME_VERSION}-${WASMTIME_PLATFORM}-c-api.${WASMTIME_EXTENSION}")
        # one URL only: CMake refuses a list that mixes a local path with web addresses
        set(WASMTIME_URL "https://github.com/bytecodealliance/wasmtime/releases/download/v${WASMTIME_VERSION}/${WASMTIME_ARCHIVE}")
        foreach(WASMTIME_DIR "${CMAKE_SOURCE_DIR}/../downloads" "$ENV{TIVOLI_DOWNLOADS}")
            if (NOT "${WASMTIME_DIR}" STREQUAL "" AND EXISTS "${WASMTIME_DIR}/${WASMTIME_ARCHIVE}")
                get_filename_component(WASMTIME_URL "${WASMTIME_DIR}/${WASMTIME_ARCHIVE}" ABSOLUTE)
            endif()
        endforeach()
        message(STATUS "wasmtime ${WASMTIME_VERSION} from ${WASMTIME_URL}")

        # The archive has no CMakeLists.txt: this only unpacks it (its one top-level folder is stripped).
        FetchContent_Declare(wasmtime
            URL "${WASMTIME_URL}"
            URL_HASH SHA256=${WASMTIME_SHA256}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(wasmtime)

        if (WIN32)
            # the DLL on Windows: the static library carries its own copy of Rust's standard library, as chat's
            # library does (TargetDarkrideChat.cmake), and MSVC's linker refuses the two in one program
            # (LNK2005, rust_eh_personality). fixup_bundle copies wasmtime.dll beside each program after its build.
            add_library(wasmtime::wasmtime SHARED IMPORTED)
            set_target_properties(wasmtime::wasmtime PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${wasmtime_SOURCE_DIR}/include"
                IMPORTED_LOCATION "${wasmtime_SOURCE_DIR}/lib/wasmtime.dll"
                IMPORTED_IMPLIB "${wasmtime_SOURCE_DIR}/lib/wasmtime.dll.lib")
            add_paths_to_fixup_libs("${wasmtime_SOURCE_DIR}/lib")
        else()
            add_library(wasmtime::wasmtime STATIC IMPORTED)
            set_target_properties(wasmtime::wasmtime PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${wasmtime_SOURCE_DIR}/include")
            set_target_properties(wasmtime::wasmtime PROPERTIES IMPORTED_LOCATION "${wasmtime_SOURCE_DIR}/lib/libwasmtime.a")
            if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
                set_target_properties(wasmtime::wasmtime PROPERTIES INTERFACE_LINK_LIBRARIES "pthread;dl;m")
            endif()
        endif()
    endif()
    target_link_libraries(${TARGET_NAME} wasmtime::wasmtime)
endmacro()
