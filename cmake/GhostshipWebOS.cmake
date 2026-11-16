# GhostshipWebOS.cmake
#
# Included by Ghostship's CMakeLists.txt (patches/0001-webos-port-build-hook.patch)
# when Web Mario 64's top-level build configures it with -DGHOSTSHIP_WEBOS_PORT=ON.
# It provides two entry points, called by the patched CMakeLists.txt:
#
#   wm64_setup_platform()      right after project(): for webOS, OpenGL ES 3, SDL-webOS,
#                              the libraries the sysroot lacks, no scripting/online;
#                              for the desktop test version, nothing beyond defaults.
#   wm64_add_port(<target>)    once the Ghostship target exists: adds the port layer
#                              (src/: transfer page, USB drives, setup screen, menu,
#                              remote control) and renames Ghostship's main() to
#                              ghostship_main(), which src/main.cpp calls.
#
# Inputs (cache variables passed by the top-level build):
#   WM64_ROOT            the Web Mario 64 repository
#   WM64_TARGET_WEBOS    ON for the TV, OFF for the desktop test version
#   WM64_VERSION         the app's version
#   WM64_PROJECT_URL     the project page, shown in the menu and on the transfer page
#   WM64_SDL2_DIR        (webOS) the SDL-webOS backport: include/, lib/libSDL2-2.0.so.0
#   WM64_GLES_STUB_DIR   (webOS) folder of the OpenGL ES 3 link stub libGLESv2.so

foreach(required WM64_ROOT WM64_VERSION WM64_PROJECT_URL)
    if(NOT ${required})
        message(FATAL_ERROR "${required} is not set: build Web Mario 64 from its top-level CMakeLists.txt")
    endif()
endforeach()

# A macro, not a function: FetchContent and the cache settings below must land in
# Ghostship's own scope.
macro(wm64_setup_platform)
    message(STATUS "Ghostship for Web Mario 64 ${WM64_VERSION} (webOS: ${WM64_TARGET_WEBOS})")

    # Mods are .o2r/.otr archives on the TV: no TinyCC runtime and no native mod libraries.
    set(ENABLE_SCRIPTING OFF CACHE BOOL "" FORCE)
    set(DISABLE_DLL_LOADER ON CACHE BOOL "" FORCE)

    if(WM64_TARGET_WEBOS)
        foreach(required WM64_SDL2_DIR WM64_GLES_STUB_DIR)
            if(NOT ${required})
                message(FATAL_ERROR "${required} is not set: build Web Mario 64 from its top-level CMakeLists.txt")
            endif()
        endforeach()

        # --- Linker: Ghostship links libultraship and cimgui with
        # $<LINK_LIBRARY:WHOLE_ARCHIVE,...>, described here for GNU ld in case the
        # platform module did not (the webOS toolchain file names the system "Buildroot").
        foreach(_wm64_lang C CXX)
            set(CMAKE_${_wm64_lang}_LINK_LIBRARY_USING_WHOLE_ARCHIVE
                "LINKER:--push-state,--whole-archive" "<LINK_ITEM>" "LINKER:--pop-state")
            set(CMAKE_${_wm64_lang}_LINK_LIBRARY_USING_WHOLE_ARCHIVE_SUPPORTED TRUE)
        endforeach()

        # --- Large files: the TV build is 32-bit ARM, where off_t is 32 bits unless
        # asked otherwise. HD texture packs are larger than 2 GiB: everything built
        # from here on (Ghostship, libultraship, libzip, the port) uses 64-bit file
        # offsets, so they can be uploaded, stored and read.
        add_compile_definitions(_FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE)

        # --- Graphics: OpenGL ES 3. libultraship's GL backend then uses GLSL ES 3.00.
        set(USE_OPENGLES ON CACHE BOOL "Enable GLES3" FORCE)
        add_compile_definitions(USE_OPENGLES=1)
        # The sysroot's GLES headers stop at ES 2: the Khronos ES 3 headers come first.
        include_directories(BEFORE SYSTEM "${WM64_ROOT}/third_party/khronos")
        # Bare "GLESv2" links (libultraship) find the ES 3 stub before the sysroot's ES 2 library;
        # at run time the TV's own libGLESv2.so (Mali, ES 3.2) is loaded.
        link_directories(BEFORE "${WM64_GLES_STUB_DIR}")
        set(OPENGL_GLESv2_LIBRARY "${WM64_GLES_STUB_DIR}/libGLESv2.so" CACHE FILEPATH "" FORCE)

        # --- SDL: the SDL-webOS backport (remote keys, Back/Exit, controllers).
        set(SDL2_DIR "${WM64_ROOT}/cmake/sdl2" CACHE PATH "" FORCE)
        find_package(SDL2 REQUIRED)

        # --- libultraship's libraries, built from source (the sysroot has only zlib).
        # Same versions as libultraship's Android build (cmake/dependencies/android.cmake);
        # OVERRIDE_FIND_PACKAGE answers its find_package() calls with them.
        include(FetchContent)
        set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
        FetchContent_Declare(nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG v3.12.0
            GIT_SHALLOW TRUE
            OVERRIDE_FIND_PACKAGE)
        set(tinyxml2_BUILD_TESTING OFF)
        FetchContent_Declare(tinyxml2
            GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
            GIT_TAG 11.0.0
            GIT_SHALLOW TRUE
            OVERRIDE_FIND_PACKAGE)
        FetchContent_Declare(spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG v1.16.0
            GIT_SHALLOW TRUE
            OVERRIDE_FIND_PACKAGE)
        # libzip: o2r archives are zip files. zlib from the sysroot, no other codecs.
        set(BUILD_TOOLS OFF)
        set(BUILD_REGRESS OFF)
        set(BUILD_EXAMPLES OFF)
        set(BUILD_DOC OFF)
        set(BUILD_OSSFUZZ OFF)
        set(BUILD_SHARED_LIBS OFF)
        set(ENABLE_BZIP2 OFF)
        set(ENABLE_LZMA OFF)
        set(ENABLE_ZSTD OFF)
        set(ENABLE_COMMONCRYPTO OFF)
        set(ENABLE_GNUTLS OFF)
        set(ENABLE_MBEDTLS OFF)
        set(ENABLE_OPENSSL OFF)
        FetchContent_Declare(libzip
            GIT_REPOSITORY https://github.com/nih-at/libzip.git
            GIT_TAG v1.11.4
            GIT_SHALLOW TRUE
            OVERRIDE_FIND_PACKAGE)
        FetchContent_MakeAvailable(nlohmann_json tinyxml2 spdlog libzip)
        # zip.h/zipconf.h for libultraship's headers, which the game includes too.
        include_directories("${libzip_SOURCE_DIR}/lib" "${libzip_BINARY_DIR}")
    endif()
endmacro()

function(wm64_add_port target)
    file(GLOB _wm64_port_sources CONFIGURE_DEPENDS "${WM64_ROOT}/src/port/*.cpp")
    target_sources(${target} PRIVATE "${WM64_ROOT}/src/main.cpp" ${_wm64_port_sources})
    # The transfer page, embedded in the executable (src/port/TransferServer.cpp serves it).
    set(_wm64_generated "${CMAKE_BINARY_DIR}/wm64-generated")
    add_custom_command(
        OUTPUT "${_wm64_generated}/WebPage.generated.h"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_wm64_generated}"
        COMMAND "${CMAKE_COMMAND}"
                -DINPUT=${WM64_ROOT}/web/index.html
                -DOUTPUT=${_wm64_generated}/WebPage.generated.h
                -DVARIABLE=kWebPageHtml
                -P "${WM64_ROOT}/cmake/EmbedTextFile.cmake"
        DEPENDS "${WM64_ROOT}/web/index.html" "${WM64_ROOT}/cmake/EmbedTextFile.cmake"
        COMMENT "Embedding the transfer page")
    target_sources(${target} PRIVATE "${_wm64_generated}/WebPage.generated.h")
    target_include_directories(${target} PRIVATE "${WM64_ROOT}/src" "${WM64_ROOT}/third_party/cpp-httplib")
    target_include_directories(${target} PRIVATE "${_wm64_generated}")
    target_compile_definitions(${target} PRIVATE
        WM64_VERSION="${WM64_VERSION}"
        WM64_PROJECT_URL="${WM64_PROJECT_URL}"
        $<$<BOOL:${WM64_TARGET_WEBOS}>:WM64_TARGET_WEBOS>)

    # Ghostship's main() runs once the port layer has the game data ready (src/main.cpp).
    set_source_files_properties("${CMAKE_SOURCE_DIR}/src/port/Game.cpp" PROPERTIES COMPILE_DEFINITIONS "main=ghostship_main")

    # The port layer watches the game's SDL calls (src/port/SdlHooks.h): menu keys,
    # buffer swaps for the menu, the audio device to pause, message boxes.
    target_link_options(${target} PRIVATE
        "LINKER:--wrap=SDL_PollEvent"
        "LINKER:--wrap=SDL_PeepEvents"
        "LINKER:--wrap=SDL_GL_SwapWindow"
        "LINKER:--wrap=SDL_OpenAudioDevice"
        "LINKER:--wrap=SDL_CreateWindow"
        "LINKER:--wrap=SDL_ShowMessageBox"
        "LINKER:--wrap=SDL_ShowSimpleMessageBox")

    if(WM64_TARGET_WEBOS)
        target_link_libraries(${target} PRIVATE SDL2::SDL2 "${WM64_GLES_STUB_DIR}/libGLESv2.so" dl)
        # libstdc++ is linked statically (the TV's is older); SDL-webOS is bundled in lib/.
        target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc)
        set_target_properties(${target} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "$ORIGIN/lib")
    endif()
endfunction()
