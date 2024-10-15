cmake_minimum_required(VERSION 3.2)

project(buddy)

option(LOCAL_TRANSPORT "Host-DPU transport method (rdma, dma)" "rdma")
if(LOCAL_TRANSPORT STREQUAL "rdma")
  set(LOCAL_RDMA)
endif()
if(LOCAL_TRANSPORT STREQUAL "dma")
  set(LOCAL_DMA)
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_FLAGS "-g -Wall ${CMAKE_CXX_FLAGS}")
set(CMAKE_C_FLAGS "-g -Wall ${CMAKE_C_FLAGS}")

find_package(MPI REQUIRED)
find_package(absl REQUIRED)

include_directories(src valgrind)

add_subdirectory(src)
add_subdirectory(test)
