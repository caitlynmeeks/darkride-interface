#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
#

# Webcam face tracking (src/FaceTrackingPlugin.h): MediaPipe's face landmarker on TensorFlow Lite (release's port,
# cmake/macros/TargetTflite.cmake), the camera through QtMultimedia. Mac and Windows (plugins/CMakeLists.txt).

set(TARGET_NAME face-tracking-plugin)

# The model: MediaPipe's face_landmarker.task (float16, version 1; Apache-2.0, Google), checked against its hash and
# embedded uncompressed in this plugin, read only while tracking is on. Downloaded once into a folder outside the build
# trees, or a local copy given with -DFACE_LANDMARKER_TASK=<file> (offline builds). A download or hash that fails stops
# the configure, so no build ships face tracking without its model.
set(FACE_LANDMARKER_URL
    "https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task")
set(FACE_LANDMARKER_SHA256 "64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff")
set(FACE_LANDMARKER_TASK "" CACHE FILEPATH
    "A local copy of MediaPipe's face_landmarker.task (float16, version 1), used instead of downloading it")

if (FACE_LANDMARKER_TASK)
  set(FACE_LANDMARKER_FILE "${FACE_LANDMARKER_TASK}")
else()
  # the downloads folder beside the source, as the other downloaded parts' (TargetJolt.cmake and friends), or
  # TIVOLI_DOWNLOADS
  if (DEFINED ENV{TIVOLI_DOWNLOADS})
    set(FACE_LANDMARKER_CACHE "$ENV{TIVOLI_DOWNLOADS}")
  else()
    set(FACE_LANDMARKER_CACHE "${CMAKE_SOURCE_DIR}/../downloads")
  endif()
  set(FACE_LANDMARKER_FILE "${FACE_LANDMARKER_CACHE}/face_landmarker-float16-1.task")
  if (EXISTS "${FACE_LANDMARKER_FILE}")
    file(SHA256 "${FACE_LANDMARKER_FILE}" FACE_LANDMARKER_FOUND_SHA256)
  endif()
  if (NOT FACE_LANDMARKER_FOUND_SHA256 STREQUAL FACE_LANDMARKER_SHA256)
    message(STATUS "Downloading MediaPipe's face model into ${FACE_LANDMARKER_CACHE}")
    file(DOWNLOAD "${FACE_LANDMARKER_URL}" "${FACE_LANDMARKER_FILE}.part" STATUS FACE_LANDMARKER_STATUS TLS_VERIFY ON)
    list(GET FACE_LANDMARKER_STATUS 0 FACE_LANDMARKER_ERROR)
    if (NOT FACE_LANDMARKER_ERROR EQUAL 0)
      list(GET FACE_LANDMARKER_STATUS 1 FACE_LANDMARKER_MESSAGE)
      file(REMOVE "${FACE_LANDMARKER_FILE}.part")
      message(FATAL_ERROR "Couldn't download MediaPipe's face model from ${FACE_LANDMARKER_URL}: "
                          "${FACE_LANDMARKER_MESSAGE}. Download it another way and configure with "
                          "-DFACE_LANDMARKER_TASK=<the file>.")
    endif()
    file(RENAME "${FACE_LANDMARKER_FILE}.part" "${FACE_LANDMARKER_FILE}")
  endif()
endif()
file(SHA256 "${FACE_LANDMARKER_FILE}" FACE_LANDMARKER_FOUND_SHA256)
if (NOT FACE_LANDMARKER_FOUND_SHA256 STREQUAL FACE_LANDMARKER_SHA256)
  message(FATAL_ERROR "${FACE_LANDMARKER_FILE} isn't MediaPipe's face_landmarker.task, float16 version 1 "
                      "(sha256 ${FACE_LANDMARKER_FOUND_SHA256}, wanted ${FACE_LANDMARKER_SHA256})")
endif()

set(FACE_LANDMARKER_QRC "${CMAKE_CURRENT_BINARY_DIR}/face-tracking.qrc")
file(WRITE "${FACE_LANDMARKER_QRC}"
     "<!DOCTYPE RCC><RCC version=\"1.0\"><qresource prefix=\"/face-tracking\">"
     "<file alias=\"face_landmarker.task\">${FACE_LANDMARKER_FILE}</file></qresource></RCC>\n")

setup_hifi_plugin(Multimedia Qml)
link_hifi_libraries(shared controllers ui plugins ui-plugins input-plugins script-engine)
include_hifi_library_headers(gpu)  # DisplayPlugin.h, for whether a headset is in use
target_tflite()

# setup_hifi_plugin builds src/'s .h, .cpp and .qrc; the model's resource and the Mac's own file are added here
qt_add_big_resources(FACE_LANDMARKER_RESOURCE "${FACE_LANDMARKER_QRC}" OPTIONS -no-compress)
target_sources(${TARGET_NAME} PRIVATE ${FACE_LANDMARKER_RESOURCE})
if (APPLE)
  # the camera permission, from AVFoundation (CameraPermission.cpp is the other systems')
  target_sources(${TARGET_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/CameraPermission_mac.mm")
  find_library(AVFoundation AVFoundation)
  target_link_libraries(${TARGET_NAME} ${AVFoundation})
endif()
