set(common_src sockets.cpp rdma.cpp dma.cpp request.cpp)
add_library(buddy_common INTERFACE)
target_link_directories(buddy_common INTERFACE /opt/mellanox/doca/lib64 /opt/mellanox/doca/lib/aarch64-linux-gnu)
target_include_directories(buddy_common INTERFACE /opt/mellanox/doca/include)
target_compile_options(buddy_common INTERFACE -D DOCA_ALLOW_EXPERIMENTAL_API)
target_link_libraries(buddy_common INTERFACE -libverbs -ldoca_common -ldoca_dma absl::flat_hash_map)

add_library(buddy_mpi SHARED ${common_src} buddy_mpi.cpp host_buffer.cpp dpu_conn.cpp)
target_link_libraries(buddy_mpi PRIVATE MPI::MPI_C buddy_common)

add_executable(buddy-proxy ${common_src} proxy_main.cpp proxy.cpp)
target_link_libraries(buddy-proxy PRIVATE buddy_common)
