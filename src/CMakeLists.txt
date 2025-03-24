set(common_src sockets.cpp rdma.cpp request.cpp)
if(LOCAL_DMA)
	set(common_src ${common_src} dma.cpp)
endif()

add_library(buddy_common INTERFACE)
target_link_libraries(buddy_common INTERFACE -libverbs)

if(LOCAL_DMA)
	target_link_directories(buddy_common INTERFACE /opt/mellanox/doca/lib64 /opt/mellanox/doca/lib/aarch64-linux-gnu)
	target_include_directories(buddy_common INTERFACE /opt/mellanox/doca/include)
	target_compile_options(buddy_common INTERFACE -D DOCA_ALLOW_EXPERIMENTAL_API)
	target_link_libraries(buddy_common INTERFACE -ldoca_common -ldoca_dma)
endif()

#if(MPI_FOUND)
#add_library(buddy_mpi SHARED ${common_src} buddy_mpi.cpp host_buffer.cpp dpu_conn.cpp)
#target_link_libraries(buddy_mpi PRIVATE MPI::MPI_C buddy_common absl::flat_hash_map)
#endif()

add_library(buddy SHARED ${common_src} host_api.cpp dpu_conn.cpp)
target_link_libraries(buddy PRIVATE buddy_common)
target_link_libraries(buddy PUBLIC MPI::MPI_C)

add_executable(buddy-proxy ${common_src} proxy_main.cpp proxy.cpp)
target_link_libraries(buddy-proxy PRIVATE buddy_common)
if(OpenMP_CXX_FOUND)
	target_link_libraries(buddy-proxy PRIVATE OpenMP::OpenMP_CXX)
endif()
