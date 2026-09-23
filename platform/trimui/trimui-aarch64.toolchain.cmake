# CMake toolchain file for TrimUI Brick Pro / CrossMix-OS (AArch64).
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=platform/trimui/trimui-aarch64.toolchain.cmake \
#         -DTRIMUI=ON -DCMAKE_BUILD_TYPE=Release ..
#
# The cross sysroot (SDL2 + Boost built by Dockerfile.aarch64) defaults to
# /opt/cross/aarch64-crossmix and can be overridden with -DTRIMUI_SYSROOT=...

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Cross toolchain (Debian/Ubuntu multiarch prefix).
set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Sysroot holding the target SDL2/Boost headers and libraries.
if(NOT DEFINED TRIMUI_SYSROOT)
    set(TRIMUI_SYSROOT "/opt/cross/aarch64-crossmix")
endif()
set(CMAKE_SYSROOT ${TRIMUI_SYSROOT})
set(CMAKE_FIND_ROOT_PATH ${TRIMUI_SYSROOT})

# Search programs on the host, but libraries/headers/packages only in sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# objcopy used to embed binary assets (boots.bin, icon64.rgba) as objects.
set(CMAKE_OBJCOPY aarch64-linux-gnu-objcopy CACHE FILEPATH "Cross objcopy")
