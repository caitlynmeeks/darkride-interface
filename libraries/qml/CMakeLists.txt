set(TARGET_NAME qml)
setup_hifi_library(Multimedia Network Qml Quick WebChannel WebSockets ${PLATFORM_QT_COMPONENTS})
link_hifi_libraries(shared networking gl)

# Required for some low level GL interaction in the OffscreenQMLSurface
target_opengl()

if (QT_VERSION_MAJOR EQUAL 6)
  # the QML surface's render interface (QRhi, entry 23 Amendment A): Qt keeps the rhi headers in Gui's private include
  # set, its own package beside Gui
  find_package(Qt6 COMPONENTS GuiPrivate REQUIRED)
  target_link_libraries(${TARGET_NAME} Qt6::GuiPrivate)
endif()
