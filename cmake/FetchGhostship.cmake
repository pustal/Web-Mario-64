# FetchGhostship.cmake
#
# Downloads Ghostship (https://github.com/HarbourMasters/Ghostship, MIT) at the
# commit this port is made for, with its submodules (libultraship, Torch). It is
# only downloaded: PatchUpstream.cmake copies and patches it, and the top-level
# CMakeLists.txt builds the patched copy.
#
# To build from a local checkout instead (e.g. while updating the patches):
#   -DFETCHCONTENT_SOURCE_DIR_GHOSTSHIP=/path/to/Ghostship
#
# Sets GHOSTSHIP_UPSTREAM_DIR.

include(FetchContent)

# Updating Ghostship: change the commit, then fix any patch that no longer applies.
set(WM64_GHOSTSHIP_COMMIT "aefb89b85e7b78daff62b491ebb2669d2d9270f0" CACHE STRING "Ghostship commit to build")

FetchContent_Declare(ghostship
    GIT_REPOSITORY https://github.com/HarbourMasters/Ghostship.git
    GIT_TAG "${WM64_GHOSTSHIP_COMMIT}"
    GIT_SUBMODULES_RECURSE ON
    GIT_PROGRESS ON
    # Download only: nothing here is added to this project.
    SOURCE_SUBDIR "do-not-add")
FetchContent_MakeAvailable(ghostship)

set(GHOSTSHIP_UPSTREAM_DIR "${ghostship_SOURCE_DIR}")
if(NOT EXISTS "${GHOSTSHIP_UPSTREAM_DIR}/libultraship/CMakeLists.txt" OR NOT EXISTS "${GHOSTSHIP_UPSTREAM_DIR}/Torch/CMakeLists.txt")
    message(FATAL_ERROR "Ghostship at ${GHOSTSHIP_UPSTREAM_DIR} lacks its submodules (libultraship, Torch)")
endif()
message(STATUS "Ghostship: ${GHOSTSHIP_UPSTREAM_DIR}")
