cmake_minimum_required(VERSION 3.10)

project(Examp)

set(CMAKE_BUILD_TYPE "Debug")

add_subdirectory(./demux_decode)
add_subdirectory(./extract_mvs)
add_subdirectory(./mux)
add_subdirectory(./remux)
add_subdirectory(./show_metadata)
add_subdirectory(./transcode)