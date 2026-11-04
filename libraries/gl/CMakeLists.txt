set(TARGET_NAME gl)
# Qt 6 moved the QOpenGL* classes (QOpenGLDebugLogger here) from QtGui to the QtOpenGL module
set(GL_QT_MODULES Gui Widgets)
if (QT_VERSION_MAJOR EQUAL 6)
  list(APPEND GL_QT_MODULES OpenGL)
endif()
setup_hifi_library(${GL_QT_MODULES})
link_hifi_libraries(shared)
target_opengl()

