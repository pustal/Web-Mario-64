# CopyIfPresent.cmake
#
# Script mode helper (cmake -P): copies SOURCE into the folder DESTINATION when
# SOURCE exists, and does nothing otherwise. Used for files a build may not have
# (gamecontrollerdb.txt is downloaded at configure time, which can fail offline).

if(NOT SOURCE OR NOT DESTINATION)
    message(FATAL_ERROR "CopyIfPresent.cmake needs SOURCE and DESTINATION")
endif()
if(EXISTS "${SOURCE}" AND NOT IS_DIRECTORY "${SOURCE}")
    file(SIZE "${SOURCE}" _size)
    if(_size GREATER 0)
        file(COPY "${SOURCE}" DESTINATION "${DESTINATION}")
    endif()
endif()
