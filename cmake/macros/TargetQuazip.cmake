#
#  Copyright 2015 High Fidelity, Inc.
#  Created by Leonardo Murillo on 2015/11/20
#
#  Distributed under the Apache License, Version 2.0.
#  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
#
# QuaZip 1.x, built with the engine against the Qt the engine builds with (Qt 5 or Qt 6), as a static library from a
# source archive pinned by its SHA-256. It replaces vcpkg's QuaZip 0.7.3, which built for Qt 5 only and was the one vcpkg
# port that needed Qt, so one vcpkg folder now serves both Qt versions. The quazip port stays in hifi-deps for older
# release lines (removing it from the list could make vcpkg uninstall it from their pinned folder).
include(FetchContent)

set(QUAZIP_VERSION 1.7.2)
set(QUAZIP_SHA256 5240f4a3475648773ba10af8b0ebe549cbfc5d6de00470a517330a80068a4f78)

macro(TARGET_QUAZIP)
  if (NOT TARGET QuaZip::QuaZip)
    # zip and unzip only: no bzip2, no tests or install, and no fetching of other libraries (zlib comes from vcpkg or
    # the system). File names: Qt 5 through QTextCodec (in Qt 5's Core; QuaZip refuses Qt 5 without it), Qt 6 as UTF-8
    # through QStringConverter, so Qt 6 needs no Core5Compat
    set(QUAZIP_QT_MAJOR_VERSION ${QT_VERSION_MAJOR} CACHE STRING "Qt major version for QuaZip" FORCE)
    set(QUAZIP_BZIP2 OFF CACHE BOOL "" FORCE)
    if (QT_VERSION_MAJOR EQUAL 6)
      set(QUAZIP_ENABLE_QTEXTCODEC OFF CACHE BOOL "" FORCE)
    else()
      set(QUAZIP_ENABLE_QTEXTCODEC ON CACHE BOOL "" FORCE)
    endif()
    set(QUAZIP_FETCH_LIBS OFF CACHE BOOL "" FORCE)
    set(QUAZIP_INSTALL OFF CACHE BOOL "" FORCE)
    set(QUAZIP_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(_QUAZIP_SAVED_SHARED ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_Declare(quazip
      URL "https://github.com/stachenov/quazip/archive/refs/tags/v${QUAZIP_VERSION}.tar.gz"
      URL_HASH SHA256=${QUAZIP_SHA256}
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(quazip)
    set(BUILD_SHARED_LIBS ${_QUAZIP_SAVED_SHARED})
    # linked into shared libraries (script-engine), so position-independent on every platform
    get_target_property(_QUAZIP_TARGET QuaZip::QuaZip ALIASED_TARGET)
    set_target_properties(${_QUAZIP_TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()
  target_link_libraries(${TARGET_NAME} QuaZip::QuaZip)
endmacro()
