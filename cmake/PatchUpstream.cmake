# PatchUpstream.cmake
#
# The port builds a patched copy of Ghostship, so the download stays exactly as
# HarbourMasters publish it. At configure time:
#   1. Ghostship (with libultraship and Torch) is copied to a staging folder,
#   2. every patches/*.patch is applied there in name order (GNU patch, -p1, from
#      Ghostship's root; each patch starts with a description of what it changes),
#   3. the result is copied into <build>/ghostship-src, touching only files whose
#      content changed, so reconfiguring does not rebuild everything.
#
# Sets GHOSTSHIP_SRC. Needs GHOSTSHIP_UPSTREAM_DIR and WM64_ROOT.

find_program(WM64_PATCH_PROGRAM patch REQUIRED)

set(GHOSTSHIP_SRC "${CMAKE_CURRENT_BINARY_DIR}/ghostship-src")
set(_wm64_staging "${CMAKE_CURRENT_BINARY_DIR}/ghostship-src-staging")
file(GLOB _wm64_patches "${WM64_ROOT}/patches/*.patch")
list(SORT _wm64_patches)

file(REMOVE_RECURSE "${_wm64_staging}")
file(MAKE_DIRECTORY "${_wm64_staging}")
# Everything but the version control data and build folders.
file(GLOB _wm64_top_entries RELATIVE "${GHOSTSHIP_UPSTREAM_DIR}" "${GHOSTSHIP_UPSTREAM_DIR}/*")
foreach(entry IN LISTS _wm64_top_entries)
    if(entry STREQUAL ".git" OR entry MATCHES "^build")
        continue()
    endif()
    file(COPY "${GHOSTSHIP_UPSTREAM_DIR}/${entry}" DESTINATION "${_wm64_staging}" PATTERN ".git" EXCLUDE)
endforeach()

foreach(patch_file IN LISTS _wm64_patches)
    execute_process(
        COMMAND "${WM64_PATCH_PROGRAM}" -p1 --forward --batch --silent -d "${_wm64_staging}" -i "${patch_file}"
        RESULT_VARIABLE _wm64_patch_result
        OUTPUT_VARIABLE _wm64_patch_output
        ERROR_VARIABLE _wm64_patch_output)
    if(NOT _wm64_patch_result EQUAL 0)
        message(FATAL_ERROR "Could not apply ${patch_file} to Ghostship "
                            "(was WM64_GHOSTSHIP_COMMIT changed?):\n${_wm64_patch_output}")
    endif()
    message(STATUS "Applied ${patch_file}")
endforeach()

file(GLOB_RECURSE _wm64_staged_files RELATIVE "${_wm64_staging}" "${_wm64_staging}/*")
foreach(relative IN LISTS _wm64_staged_files)
    get_filename_component(_wm64_target_dir "${GHOSTSHIP_SRC}/${relative}" DIRECTORY)
    file(MAKE_DIRECTORY "${_wm64_target_dir}")
    file(COPY_FILE "${_wm64_staging}/${relative}" "${GHOSTSHIP_SRC}/${relative}" ONLY_IF_DIFFERENT)
endforeach()
file(REMOVE_RECURSE "${_wm64_staging}")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_wm64_patches})
