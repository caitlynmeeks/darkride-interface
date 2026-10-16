# Common Ambient Variables:
#   VCPKG_ROOT_DIR = <C:\path\to\current\vcpkg>
#   TARGET_TRIPLET is the current triplet (x86-windows, etc)
#   PORT is the current port name (zlib, etc)
#   CURRENT_BUILDTREES_DIR = ${VCPKG_ROOT_DIR}\buildtrees\${PORT}
#   CURRENT_PACKAGES_DIR  = ${VCPKG_ROOT_DIR}\packages\${PORT}_${TARGET_TRIPLET}
#

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO vircadia/nvidia-texture-tools
    REF d8b7a98aeb177b5eddb76571183bbd2f95d54e6c # 2021-07-04, already includes the arm64 Debug.cpp fix
    SHA512 ea15ffd19eb1e14c8ebd62f8d7de3df1ecf6c18a339025f4a0e13419717d510903fc126ec6d1bdfbb5a2f4525a922412b72318bc8dd55dd000481a3924fbfcd4
    HEAD_REF master
    PATCHES
        fix-arm64-darwin.patch
)

# MSVC has had log2f and exp2f since Visual Studio 2013, and Visual Studio 2022 refuses a definition of
# the log2f intrinsic, so keep nvmath's own versions for older compilers only (nvmath.h mixes line ends).
file(READ ${SOURCE_PATH}/src/nvmath/nvmath.h NVMATH_H)
string(REGEX REPLACE "#if NV_CC_MSVC(\r?\n)NV_FORCEINLINE float log2f"
    "#if NV_CC_MSVC && _MSC_VER < 1800\\1NV_FORCEINLINE float log2f" NVMATH_H "${NVMATH_H}")
file(WRITE ${SOURCE_PATH}/src/nvmath/nvmath.h "${NVMATH_H}")

vcpkg_configure_cmake(
    SOURCE_PATH ${SOURCE_PATH}
    # Ninja rather than a Visual Studio generator, which vcpkg's CMake 3.21 names "Visual Studio 17 2022"
    # and so cannot find a Visual Studio 2026 install
    PREFER_NINJA
    OPTIONS
        -DBUILD_TESTS=OFF
        -DBUILD_TOOLS=OFF
)

vcpkg_install_cmake()

if(VCPKG_LIBRARY_LINKAGE STREQUAL static)
    file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/bin ${CURRENT_PACKAGES_DIR}/debug/bin)
endif()
    
vcpkg_copy_pdbs()

file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug/include)
file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug/share)

# Handle copyright
file(REMOVE ${CURRENT_PACKAGES_DIR}/share/doc/nvtt/LICENSE)
file(COPY ${SOURCE_PATH}/LICENSE DESTINATION ${CURRENT_PACKAGES_DIR}/share/nvtt)
file(RENAME ${CURRENT_PACKAGES_DIR}/share/nvtt/LICENSE ${CURRENT_PACKAGES_DIR}/share/nvtt/copyright)
