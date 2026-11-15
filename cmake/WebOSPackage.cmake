# WebOSPackage.cmake
#
# Adds the "webos-stage" and "webos-package" targets. The IPK layout, staged
# under <build>/ipk/:
#   appinfo.json, icon.png, largeIcon.png
#   WebMario64                  the game with the port layer (Ghostship's executable, stripped)
#   lib/libSDL2-2.0.so.0        the bundled SDL-webOS
#   ghostship.o2r               Ghostship's own assets (fonts, shaders, icons; no game data)
#   config.yml, assets/         the descriptions Ghostship's extractor reads to make sm64.o2r from the ROM
#   gamecontrollerdb.txt        SDL controller mappings (if it could be downloaded)
#   LICENSE, THIRD_PARTY_NOTICES.md, licenses/
# Ghostship looks for ghostship.o2r, config.yml and assets/ next to its executable.
# Then dist/<id>_<version>_arm.ipk is built with ares-package (@webos-tools/cli),
# with the Homebrew Channel manifest next to it.
#
# No game data is packaged: the player's ROM becomes sm64.o2r on the TV.

find_program(WM64_ARES_PACKAGE ares-package)

set(WM64_IPK_STAGE "${CMAKE_CURRENT_BINARY_DIR}/ipk")
set(WM64_DIST_DIR "${WM64_ROOT}/dist")
set(WM64_IPK_NAME "${WM64_APP_ID}_${PROJECT_VERSION}_arm.ipk")
set(_wm64_inner "${WM64_GHOSTSHIP_BINARY_DIR}")

configure_file("${WM64_ROOT}/webos/appinfo.json.in" "${CMAKE_CURRENT_BINARY_DIR}/appinfo.json" @ONLY)

if(NOT CMAKE_STRIP)
    set(CMAKE_STRIP strip)
endif()

add_custom_target(webos-stage
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${WM64_IPK_STAGE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${WM64_IPK_STAGE}/lib" "${WM64_IPK_STAGE}/licenses"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/appinfo.json" "${WM64_IPK_STAGE}/"
    COMMAND "${CMAKE_COMMAND}" -E copy "${WM64_ROOT}/webos/icon.png" "${WM64_ROOT}/webos/largeIcon.png" "${WM64_IPK_STAGE}/"
    COMMAND "${CMAKE_COMMAND}" -E copy "${_wm64_inner}/Ghostship" "${WM64_IPK_STAGE}/WebMario64"
    COMMAND "${CMAKE_STRIP}" --strip-unneeded "${WM64_IPK_STAGE}/WebMario64"
    COMMAND "${CMAKE_COMMAND}" -E copy "${WM64_SDL2_RUNTIME_LIBRARY}" "${WM64_IPK_STAGE}/lib/"
    COMMAND "${CMAKE_STRIP}" --strip-unneeded "${WM64_IPK_STAGE}/lib/libSDL2-2.0.so.0"
    COMMAND "${CMAKE_COMMAND}" -E copy "${_wm64_inner}/ghostship.o2r" "${GHOSTSHIP_SRC}/config.yml" "${WM64_IPK_STAGE}/"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${GHOSTSHIP_SRC}/assets" "${WM64_IPK_STAGE}/assets"
    COMMAND "${CMAKE_COMMAND}" -DSOURCE=${_wm64_inner}/gamecontrollerdb.txt -DDESTINATION=${WM64_IPK_STAGE}
            -P "${WM64_ROOT}/cmake/CopyIfPresent.cmake"
    COMMAND "${CMAKE_COMMAND}" -E copy "${WM64_ROOT}/LICENSE" "${WM64_ROOT}/THIRD_PARTY_NOTICES.md" "${WM64_IPK_STAGE}/"
    COMMAND "${CMAKE_COMMAND}" -E copy "${GHOSTSHIP_SRC}/LICENSE.md" "${WM64_IPK_STAGE}/licenses/Ghostship-LICENSE.md"
    COMMAND "${CMAKE_COMMAND}" -E copy "${GHOSTSHIP_SRC}/libultraship/LICENSE" "${WM64_IPK_STAGE}/licenses/libultraship-LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E copy "${GHOSTSHIP_SRC}/Torch/LICENSE" "${WM64_IPK_STAGE}/licenses/Torch-LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E copy "${WM64_ROOT}/third_party/cpp-httplib/LICENSE" "${WM64_IPK_STAGE}/licenses/cpp-httplib-LICENSE"
    DEPENDS ghostship
    COMMENT "Staging the IPK contents in ${WM64_IPK_STAGE}"
    VERBATIM)

if(WM64_ARES_PACKAGE)
    add_custom_target(webos-package
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${WM64_DIST_DIR}"
        COMMAND "${WM64_ARES_PACKAGE}" --no-minify "${WM64_IPK_STAGE}" -o "${WM64_DIST_DIR}"
        COMMAND "${CMAKE_COMMAND}"
                -DIPK=${WM64_DIST_DIR}/${WM64_IPK_NAME}
                -DOUTPUT=${WM64_DIST_DIR}/${WM64_APP_ID}.manifest.json
                -DAPP_ID=${WM64_APP_ID}
                -DVERSION=${PROJECT_VERSION}
                "-DTITLE=Web Mario 64"
                "-DDESCRIPTION=Super Mario 64 (Ghostship, from the decompilation) for webOS. Bring your own ROM."
                -DICON_URI=https://raw.githubusercontent.com/pustal/Web-Mario-64/main/webos/largeIcon.png
                -DSOURCE_URL=https://github.com/pustal/Web-Mario-64
                -P "${WM64_ROOT}/cmake/WriteManifest.cmake"
        DEPENDS webos-stage
        COMMENT "Building ${WM64_IPK_NAME}"
        VERBATIM)
else()
    add_custom_target(webos-package
        COMMAND "${CMAKE_COMMAND}" -E echo "ares-package not found: npm install -g @webos-tools/cli"
        COMMAND "${CMAKE_COMMAND}" -E false
        DEPENDS webos-stage)
endif()
