add_library(buddy_common STATIC sockets.cpp rdma.cpp dma.cpp request.cpp)
target_link_directories(buddy_common INTERFACE /opt/mellanox/doca/lib64 /opt/mellanox/doca/lib/aarch64-linux-gnu)
target_include_directories(buddy_common PRIVATE /opt/mellanox/doca/include)
target_compile_options(buddy_common PRIVATE -D DOCA_ALLOW_EXPERIMENTAL_API -fPIC)
target_link_libraries(buddy_common PRIVATE -libverbs -ldoca_common -ldoca_dma absl::flat_hash_map)

add_library(buddy_mpi SHARED buddy_mpi.cpp host_buffer.cpp dpu_conn.cpp)
target_link_libraries(buddy_mpi PRIVATE MPI::MPI_C buddy_common)

add_library(buddy_proxy STATIC proxy.cpp)
target_link_libraries(buddy_proxy PUBLIC buddy_common)

add_executable(buddy-proxy proxy_main.cpp)
target_link_libraries(buddy-proxy PRIVATE buddy_proxy)
