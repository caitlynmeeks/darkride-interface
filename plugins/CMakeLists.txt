#
#  Created by Bradley Austin Davis on 2015/10/25
#  Copyright 2015 High Fidelity, Inc.
#
#  Distributed under the Apache License, Version 2.0.
#  See the accompanying file LICENSE or http:#www.apache.org/licenses/LICENSE-2.0.html
#

# add the plugin directories
file(GLOB PLUGIN_SUBDIRS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*")
list(REMOVE_ITEM PLUGIN_SUBDIRS "CMakeFiles")

# client-side plugins
if (NOT SERVER_ONLY AND NOT ANDROID)
  # The Oculus PC SDK (LibOVR) archive is no longer downloadable, so vcpkg leaves it out;
  # the plugin is built only when LibOVR has been put in the vcpkg tree by hand.
  # Rift and Quest headsets still work through SteamVR with the openvr plugin.
  if (EXISTS "${VCPKG_INSTALL_ROOT}/lib/LibOVR.lib")
    set(DIR "oculus-plugin")
    add_subdirectory(${DIR})
  endif()
  set(DIR "sdl2-plugin")
  add_subdirectory(${DIR})
  set(DIR "openvr-plugin")
  add_subdirectory(${DIR})
  set(DIR "oculus-legacy-plugin")
  add_subdirectory(${DIR})

  if (USE_SIXENSE)
    set(DIR "sixense-plugin")
    add_subdirectory(${DIR})
  endif()
  
  # hopefully we can eventually rewrite windows and macos
  set(DIR "spacemouse-plugin")
  add_subdirectory(${DIR})
  set(DIR "spacemouse-linux-plugin")
  add_subdirectory(${DIR})

  # The Neuron SDK is a prebuilt x86_64 binary; there is none for arm64 macOS.
  if (NOT (APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64"))
    set(DIR "neuron-plugin")
    add_subdirectory(${DIR})
  endif()
  set(DIR "kinect-plugin")
  add_subdirectory(${DIR})

  if (NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(DIR "steam-plugin")
    add_subdirectory(${DIR})
    # The Leap Motion V2 SDK for Windows came from cdn.tivolicloud.com (gone)
    if (NOT APPLE AND NOT WIN32)
      set(DIR "leapmotion-plugin")
      add_subdirectory(${DIR})
    endif()
  endif()

  if (WIN32)
    set(DIR "osc-plugin")
    add_subdirectory(${DIR})
  endif()

endif()

# server-side plugins

if (NOT DISABLE_PCM_CODEC)
  set(DIR "pcm-codec-plugin")
  add_subdirectory(${DIR})
else()
  message(STATUS "PCM codec disabled")
endif()

if (NOT DISABLE_HIFI_CODEC)
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    message(STATUS "Hifi codec unavailable for arm64")
  else()
    set(DIR "hifi-codec-plugin")
    add_subdirectory(${DIR})
  endif()
else()
  message(STATUS "Hifi codec disabled")
endif()

set(DIR "tea-protocol-plugin")
if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/${DIR}")
  add_subdirectory(${DIR})
elseif (EXISTS "${CMAKE_CURRENT_LIST_DIR}/tea-protocol-plugin-prebuilt/tea-protocol-plugin-2021-12-12"
        AND NOT (APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64"))
  # the prebuilt plugin's archive came from cdn.tivolicloud.com, which is gone; it is used when someone
  # has extracted a copy beside its CMakeLists.txt (it was x86_64 only)
  set(DIR "tea-protocol-plugin-prebuilt")
  add_subdirectory(${DIR})
else()
  message(STATUS "Tea protocol plugin unavailable (its prebuilt archive is no longer downloadable); skipping")
endif()

# example plugins
# set(DIR "js-api-example-plugin")
# add_subdirectory(${DIR})
