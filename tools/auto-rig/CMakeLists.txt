set(TARGET_NAME auto-rig)
setup_hifi_project(Core Gui)
setup_memory_debugger()
link_hifi_libraries(shared networking fbx hfm graphics gpu auto-rigging animation)

include_hifi_library_headers(image)
