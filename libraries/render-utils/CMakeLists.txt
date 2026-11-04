set(TARGET_NAME render-utils)

# pull in the resources.qrc file
qt_add_resources(QT_RESOURCES_FILE "${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/fonts.qrc")
# Qt 6 moved QOpenGLFramebufferObject (FboCache) from QtGui to the QtOpenGL module
set(RENDER_UTILS_QT_MODULES Gui Network Qml Quick Script)
if (QT_VERSION_MAJOR EQUAL 6)
  list(APPEND RENDER_UTILS_QT_MODULES OpenGL)
endif()
setup_hifi_library(${RENDER_UTILS_QT_MODULES})
link_hifi_libraries(shared task ktx gpu shaders graphics graphics-scripting material-networking model-networking render animation fbx image procedural)
include_hifi_library_headers(audio)
include_hifi_library_headers(networking)
include_hifi_library_headers(octree)
include_hifi_library_headers(hfm)

# tell CMake to exclude qrc_fonts.cpp for policy CMP0071
set_property(SOURCE qrc_fonts.cpp PROPERTY SKIP_AUTOMOC ON)

if (NOT ANDROID)
  target_nsight()
endif ()

