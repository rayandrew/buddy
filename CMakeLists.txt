cmake_minimum_required(VERSION 3.2)

project(buddy)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "-Wall ${CMAKE_CXX_FLAGS}")

find_package(MPI REQUIRED)
include_directories(src)

add_subdirectory(src)
add_subdirectory(bin)
