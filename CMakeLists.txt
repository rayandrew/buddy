cmake_minimum_required(VERSION 3.2)

project(buddy)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_FLAGS "-g -Wall ${CMAKE_CXX_FLAGS}")

find_package(MPI REQUIRED)
find_package(absl REQUIRED)

include_directories(src valgrind)

add_subdirectory(src)
add_subdirectory(bin)
