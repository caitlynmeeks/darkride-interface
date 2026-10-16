#
#  Created on 2026-10-16 for Quest 3 hand tracking over Link (Meta's PC OpenXR runtime).
#
#  Distributed under the Apache License, Version 2.0.
#  See the accompanying file LICENSE or http:#www.apache.org/licenses/LICENSE-2.0.html
#

# Windows only for now: the session needs a WGL context. Linux would need a GLX or EGL binding.
if (WIN32 AND NOT USE_GLES)
    set(TARGET_NAME openxr-plugin)
    setup_hifi_plugin(Gui Qml Multimedia)
    link_hifi_libraries(shared task gl qml networking controllers ui
        plugins display-plugins ui-plugins input-plugins script-engine
        audio-client render-utils graphics shaders gpu render material-networking model-networking model-baker hfm fbx ktx image procedural ${PLATFORM_GL_BACKEND})
    include_hifi_library_headers(octree)

    target_openxr()
    # audio-client and webrtc use the multimedia timer and wave APIs
    target_link_libraries(${TARGET_NAME} Winmm.lib)
endif()
