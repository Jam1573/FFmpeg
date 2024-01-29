cmake_minimum_required(VERSION 3.10)

message("Builde remux ...... ")

project(remux)

file(GLOB SRC ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

set(EXECUTABLE_OUTPUT_PATH ${EXE_PATH})

add_executable(${PROJECT_NAME} ${SRC})

target_include_directories(${PROJECT_NAME} PRIVATE /usr/local/ffmpeg/include)
target_link_directories(${PROJECT_NAME} PRIVATE /usr/local/ffmpeg/lib)
target_link_libraries(${PROJECT_NAME} PRIVATE avutil avformat)
