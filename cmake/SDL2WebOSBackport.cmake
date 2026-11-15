# SDL2WebOSBackport.cmake
#
# Downloads, for webOS builds, webosbrew's SDL-webOS
# backport (https://github.com/webosbrew/SDL-webOS), the same prebuilt SDL2 that
# Moonlight TV ships. Compared with the SDL2 in the TV firmware it adds the
# remote control scancodes (SDL_SCANCODE_WEBOS_*), the hints that deliver the
# Back/Exit keys to the app, and up-to-date game controller support.
#
# The shared library is installed next to the app (lib/) and found via RPATH.
#
# Sets WM64_SDL2_DIR (headers in include/SDL2, library in lib/) and
# WM64_SDL2_RUNTIME_LIBRARY; target wm64_sdl2_backport downloads it.

include(ExternalProject)

set(WM64_SDL2_BACKPORT_RELEASE "release-2.30.12-webos.6" CACHE STRING "SDL-webOS release to bundle")
# Hash of the release above; clear it if you deliberately switch releases.
set(WM64_SDL2_BACKPORT_SHA256 "63dc9ea268bdf64801fce5e0c19585ee6aaf79f0b08486c853c47f922385c698"
    CACHE STRING "SHA-256 of the SDL-webOS archive (empty to skip verification)")

string(REGEX REPLACE "^release-([0-9]+\\.[0-9]+\\.[0-9]+)-webos\\.[0-9]+$" "SDL2-\\1-webos.tar.gz"
       _wm64_sdl2_archive "${WM64_SDL2_BACKPORT_RELEASE}")

set(_wm64_sdl2_hash_args)
if(WM64_SDL2_BACKPORT_SHA256)
    set(_wm64_sdl2_hash_args URL_HASH "SHA256=${WM64_SDL2_BACKPORT_SHA256}")
endif()

ExternalProject_Add(wm64_sdl2_backport
    URL "https://github.com/webosbrew/SDL-webOS/releases/download/${WM64_SDL2_BACKPORT_RELEASE}/${_wm64_sdl2_archive}"
    ${_wm64_sdl2_hash_args}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ${CMAKE_COMMAND} -E copy_directory <SOURCE_DIR> <INSTALL_DIR>
    BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libSDL2-2.0.so.0
)
ExternalProject_Get_Property(wm64_sdl2_backport INSTALL_DIR)

# The include directory only exists after the download; create it so CMake accepts it now.
file(MAKE_DIRECTORY "${INSTALL_DIR}/include/SDL2")

# Handed to the Ghostship build (cmake/GhostshipWebOS.cmake makes SDL2::SDL2 from it).
set(WM64_SDL2_DIR "${INSTALL_DIR}")
# The library file bundled into the IPK.
set(WM64_SDL2_RUNTIME_LIBRARY "${INSTALL_DIR}/lib/libSDL2-2.0.so.0")
