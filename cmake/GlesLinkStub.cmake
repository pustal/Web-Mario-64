# GlesLinkStub.cmake
#
# Provides the target wm64_gles_link_stub (alias WM64::GLESv2): a link-time
# libGLESv2.so with every OpenGL ES 3 entry point.
#
# The webOS sysroot's libGLESv2.so only exports ES 2 functions, while the TV's
# real Mali driver exports the full ES 3.2 set (checked on the TV with the
# ES3-WebOS-probe). We build a stub shared library from the Khronos header
# with every ES 3 entry point and the same soname as the real library, and link
# against it. The executable then records "libGLESv2.so" as a dependency and
# the TV's own driver is loaded at run time. The stub is never packaged.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_wm64_gles_stub_source "${CMAKE_CURRENT_BINARY_DIR}/gles_stub/gles3_stub.c")
add_custom_command(
    OUTPUT "${_wm64_gles_stub_source}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/gles_stub"
    COMMAND "${Python3_EXECUTABLE}" "${WM64_ROOT}/tools/gen_gles_stub.py"
            "${WM64_ROOT}/third_party/khronos/GLES3/gl3.h" "${_wm64_gles_stub_source}"
    DEPENDS "${WM64_ROOT}/tools/gen_gles_stub.py" "${WM64_ROOT}/third_party/khronos/GLES3/gl3.h"
    COMMENT "Generating the OpenGL ES 3 link stub")

add_library(wm64_gles_link_stub SHARED "${_wm64_gles_stub_source}")
set_target_properties(wm64_gles_link_stub PROPERTIES
    OUTPUT_NAME "GLESv2"
    NO_SONAME OFF
    # Same soname as the TV's library, so the executable asks for the real one.
    LINK_FLAGS "-Wl,-soname,libGLESv2.so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gles_stub")

add_library(WM64::GLESv2 ALIAS wm64_gles_link_stub)
