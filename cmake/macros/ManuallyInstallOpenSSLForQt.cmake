#
#  ManuallyInstallOpenSSLforQt.cmake
#
#  Created by Stephen Birarda on 1/15/16.
#  Copyright 2014 High Fidelity, Inc.
#
#  Distributed under the Apache License, Version 2.0.
#  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
#

macro(manually_install_openssl_for_qt)

  # Qt dynamically links OpenSSL if it can find it on the user's machine
  # We want to avoid it being found somewhere random and have it not being a compatible version
  # So even though we don't need the dynamic version of OpenSSL for our direct-use purposes
  # we use this macro to include the two SSL DLLs with the targets using QtNetwork
  if (WIN32)
    # we have to call find_package(OpenSSL) here even though this target may not directly need it
    find_package(OpenSSL REQUIRED)

    set(_OPENSSL_DLLS "${VCPKG_INSTALL_ROOT}/bin/libssl-1_1-x64.dll" "${VCPKG_INSTALL_ROOT}/bin/libcrypto-1_1-x64.dll")

    install(
      FILES ${_OPENSSL_DLLS}
      DESTINATION ${TARGET_INSTALL_DIR}
      COMPONENT ${TARGET_INSTALL_COMPONENT}
    )

    # fixup_bundle only copies the DLLs an executable imports, and an executable that never calls libssl itself
    # doesn't import it, so copy both beside the executable for Qt to load at runtime
    get_target_property(_OPENSSL_TARGET_TYPE ${TARGET_NAME} TYPE)
    if (_OPENSSL_TARGET_TYPE STREQUAL "EXECUTABLE")
      add_custom_command(
        TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_OPENSSL_DLLS} "$<TARGET_FILE_DIR:${TARGET_NAME}>"
      )
    endif ()
  endif()

endmacro()
