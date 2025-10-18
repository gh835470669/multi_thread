cmake_minimum_required(VERSION 3.25)

project(multi_thread)

set(CMAKE_CXX_STANDARD 26)

# source files
file(GLOB_RECURSE SOURCES src/*.cpp src/*.h src/*.hpp)

add_executable(multi_thread ${SOURCES})
