# Web Mario 64: Super Mario 64 for LG webOS TVs, built from Ghostship
# (HarbourMasters' Super Mario 64 PC port, https://github.com/HarbourMasters/Ghostship).
#
#   cmake -B build-webos -G Ninja -DCMAKE_TOOLCHAIN_FILE=<sdk>/share/buildroot/toolchainfile.cmake
#   cmake --build build-webos                          # the game
#   cmake --build build-webos --target webos-package   # dist/<id>_<version>_arm.ipk
#
# How it fits together:
#  - Ghostship is downloaded at a pinned commit (cmake/FetchGhostship.cmake), copied
#    into the build folder and patched there (cmake/PatchUpstream.cmake, patches/).
#  - The patched Ghostship is built as its own CMake project (ExternalProject) with
#    the webOS toolchain. Its patched CMakeLists.txt includes cmake/GhostshipWebOS.cmake,
#    which supplies SDL-webOS, OpenGL ES 3, the libraries the TV lacks, and the port
#    layer in src/ (transfer page, USB drives, setup screen, menu, remote control).
#  - No game data is built in: on the TV, the player's ROM becomes sm64.o2r (Ghostship's
#    asset archive) with Ghostship's own extractor (Torch).
#
# Without the webOS toolchain, the same project builds a desktop test version
# (SDL2 and OpenGL from the system), used to try the port layer under Xvfb.

cmake_minimum_required(VERSION 3.24)
project(WebMario64 VERSION 1.1.2 LANGUAGES C CXX)

set(WM64_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${WM64_ROOT}/cmake")

set(WM64_APP_ID "org.webmario64.app" CACHE STRING "webOS application id")
set(WM64_PROJECT_URL "https://github.com/pustal/Web-Mario-64/" CACHE STRING "Shown in the menu and on the transfer page")

execute_process(COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
                OUTPUT_VARIABLE _wm64_target OUTPUT_STRIP_TRAILING_WHITESPACE)
if(_wm64_target MATCHES "webos")
    set(WM64_TARGET_WEBOS ON)
    message(STATUS "Building for webOS (${_wm64_target})")
else()
    set(WM64_TARGET_WEBOS OFF)
    message(STATUS "Building the desktop test version (${_wm64_target})")
endif()

include(FetchGhostship)   # GHOSTSHIP_UPSTREAM_DIR
include(PatchUpstream)    # GHOSTSHIP_SRC: the patched copy

set(_wm64_inner_args
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DWM64_ROOT=${WM64_ROOT}
    -DWM64_TARGET_WEBOS=${WM64_TARGET_WEBOS}
    -DWM64_VERSION=${PROJECT_VERSION}
    -DWM64_PROJECT_URL=${WM64_PROJECT_URL}
    -DGHOSTSHIP_WEBOS_PORT=ON
    # No online features: there is no Satella relay on a TV.
    -DUSE_SATELLA=OFF
    -DUSE_NETWORKING=OFF)

if(WM64_TARGET_WEBOS)
    include(SDL2WebOSBackport)   # WM64_SDL2_DIR, target wm64_sdl2_backport
    include(GlesLinkStub)        # target wm64_gles_link_stub (link-time libGLESv2 with ES 3)
    list(APPEND _wm64_inner_args
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        # The webOS toolchain file calls the system "Buildroot"; Ghostship and libultraship
        # choose their Linux code paths by CMAKE_SYSTEM_NAME, which is what the TV runs.
        -DCMAKE_SYSTEM_NAME=Linux
        -DWM64_SDL2_DIR=${WM64_SDL2_DIR}
        -DWM64_GLES_STUB_DIR=${CMAKE_CURRENT_BINARY_DIR}/gles_stub)
    set(_wm64_inner_depends wm64_sdl2_backport wm64_gles_link_stub)
endif()

include(ExternalProject)
set(WM64_GHOSTSHIP_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/ghostship")
ExternalProject_Add(ghostship
    SOURCE_DIR "${GHOSTSHIP_SRC}"
    BINARY_DIR "${WM64_GHOSTSHIP_BINARY_DIR}"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS ${_wm64_inner_args}
    # The port layer and patched sources change between builds: always let the inner build decide.
    BUILD_ALWAYS ON
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target Ghostship GeneratePortO2R
    INSTALL_COMMAND ""
    DEPENDS ${_wm64_inner_depends}
    USES_TERMINAL_BUILD ON)

if(WM64_TARGET_WEBOS)
    include(WebOSPackage)        # webos-stage, webos-package
endif()
