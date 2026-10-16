# The OpenXR-SDK repository ships the loader's generated sources, so no Python is needed.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO KhronosGroup/OpenXR-SDK
    REF release-1.1.62
    SHA512 4bd17007ba79ddd20bfa04d310cc756f6a135804dc5be99e35b70c83baecebc75b931bbc055ab90942412eae324a6f6943ed0403e8f9cbd33ef48bf28f3a181e
    HEAD_REF main
)

# Windows builds the loader as a static library by default, so nothing extra has to be shipped with the plugin.
vcpkg_configure_cmake(
    SOURCE_PATH ${SOURCE_PATH}
    PREFER_NINJA
    OPTIONS
        -DBUILD_API_LAYERS=OFF
        -DBUILD_TESTS=OFF
        -DBUILD_CONFORMANCE_TESTS=OFF
        -DBUILD_FORCE_GENERATION=OFF
        -DCMAKE_DISABLE_FIND_PACKAGE_Python3=ON
)

vcpkg_install_cmake()

if(VCPKG_TARGET_IS_WINDOWS)
    vcpkg_fixup_cmake_targets(CONFIG_PATH cmake TARGET_PATH share/openxr)
else()
    vcpkg_fixup_cmake_targets(CONFIG_PATH lib/cmake/openxr TARGET_PATH share/openxr)
endif()

file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug/include ${CURRENT_PACKAGES_DIR}/debug/share)

file(INSTALL ${SOURCE_PATH}/LICENSE DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT} RENAME copyright)
