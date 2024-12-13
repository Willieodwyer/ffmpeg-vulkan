if ($ENV{VCPKG_ROOT} STREQUAL "")
    message(FATAL_ERROR "You must set VCPKG_ROOT to be the root of your vcpkg installation.")
endif ()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "vcpkg toolchain file" FORCE)
message(STATUS "Using cmake toolchain: ${CMAKE_TOOLCHAIN_FILE}")