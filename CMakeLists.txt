cmake_minimum_required(VERSION 3.14)

include(vcpkg.cmake)

project(ffmpeg-vulkan C CXX)
set(CMAKE_CXX_STANDARD 14)

#find_package(FFMPEG REQUIRED)
find_package(Vulkan REQUIRED)

add_subdirectory(src)

add_executable(example ${SOURCES}
        src/SoftwareScale.h)
target_link_libraries(example PRIVATE
        avcodec
        avutil
        avformat
        swscale
        avfilter)

#target_include_directories(example PRIVATE ${FFMPEG_INCLUDE_DIRS})
#target_link_directories(example PRIVATE ${FFMPEG_LIBRARY_DIRS})
#target_link_libraries(example PRIVATE ${FFMPEG_LIBRARIES})

target_link_libraries(example PRIVATE Vulkan::Vulkan)
