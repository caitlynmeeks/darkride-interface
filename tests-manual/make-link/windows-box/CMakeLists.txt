#
#  CMakeLists.txt
#  tests-manual/make-link/windows-box
#
#  Make v1 on Windows (6b): the stand-alone Blender box test, built on its own (not part of Darkride's build):
#    cmake -S . -B build -G Ninja && cmake --build build
#    build\make-box-test.exe "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" <test folder>
#
#  Copyright 2026 Darkride.
#
#  Distributed under the MIT License.
#
cmake_minimum_required(VERSION 3.20)
project(make-box-test CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(make-box-test make-box-test.cpp)
target_compile_definitions(make-box-test PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000C)
target_link_libraries(make-box-test PRIVATE userenv advapi32 ws2_32 bcrypt)
configure_file(probe.py ${CMAKE_CURRENT_BINARY_DIR}/probe.py COPYONLY)
