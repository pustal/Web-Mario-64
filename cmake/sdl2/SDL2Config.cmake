# SDL2Config.cmake
#
# Answers find_package(SDL2) inside the Ghostship build (libultraship and its
# ImGui backend ask for it) with the SDL-webOS backport that the outer build
# downloaded (cmake/SDL2WebOSBackport.cmake). WM64_SDL2_DIR is that backport's
# install folder: include/SDL2/*.h and lib/libSDL2-2.0.so.0.

if(NOT WM64_SDL2_DIR)
    message(FATAL_ERROR "WM64_SDL2_DIR is not set: build Web-Mario-64 from its top-level CMakeLists.txt")
endif()

set(SDL2_INCLUDE_DIRS "${WM64_SDL2_DIR}/include/SDL2" "${WM64_SDL2_DIR}/include")
set(SDL2_INCLUDE_DIR "${WM64_SDL2_DIR}/include/SDL2")
set(SDL2_LIBRARIES SDL2::SDL2)
set(SDL2_FOUND TRUE)

if(NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 SHARED IMPORTED GLOBAL)
    set_target_properties(SDL2::SDL2 PROPERTIES
        IMPORTED_LOCATION "${WM64_SDL2_DIR}/lib/libSDL2-2.0.so.0"
        IMPORTED_SONAME "libSDL2-2.0.so.0"
        INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIRS}"
        INTERFACE_COMPILE_DEFINITIONS "__WEBOS__")
endif()
# Some code links SDL2::SDL2main; on Linux there is none, so it is an empty target.
if(NOT TARGET SDL2::SDL2main)
    add_library(SDL2::SDL2main INTERFACE IMPORTED GLOBAL)
endif()
