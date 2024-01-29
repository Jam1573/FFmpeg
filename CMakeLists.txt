# 用于 FFmpeg/custom 编译
cmake_minimum_required(VERSION 3.10)

message("Build FFmpeg/custom: ")

project(FFCustom)

set(EXE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/build/bin)

add_subdirectory(./custom)
