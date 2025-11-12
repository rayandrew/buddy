cmake_minimum_required(VERSION 3.13)

project(buddy)

option(LOCAL_TRANSPORT "Host-DPU transport method (rdma, dma)" "rdma")
if(LOCAL_TRANSPORT STREQUAL "rdma")
  set(LOCAL_RDMA)
endif()
if(LOCAL_TRANSPORT STREQUAL "dma")
  set(LOCAL_DMA)
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_FLAGS "-Wall ${CMAKE_CXX_FLAGS}")
set(CMAKE_C_FLAGS "-Wall ${CMAKE_C_FLAGS}")

find_package(MPI REQUIRED)
find_package(absl)
find_package(OpenMP)

add_library(hpctoolkit INTERFACE)
if(HPCTOOLKIT)
    target_link_libraries(hpctoolkit INTERFACE -lhpctoolkit)
    target_compile_options(hpctoolkit INTERFACE -D HAVE_HPCTOOLKIT)
    target_link_options(hpctoolkit INTERFACE -L "${HPCTOOLKIT}/lib/hpctoolkit")
endif()

include_directories(src valgrind)

add_subdirectory(src)
add_subdirectory(test)

option(BUILD_APPS "Build apps/")
if(BUILD_APPS)
  add_subdirectory(apps)
endif()
