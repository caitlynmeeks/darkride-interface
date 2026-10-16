#
#  Created on 2026-10-16 for the OpenXR plugin.
#
#  Distributed under the Apache License, Version 2.0.
#  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
#
macro(TARGET_OPENXR)
    # the Khronos loader from vcpkg (cmake/ports/openxr-loader)
    find_package(OpenXR CONFIG REQUIRED)
    target_link_libraries(${TARGET_NAME} OpenXR::openxr_loader OpenXR::headers)
endmacro()
