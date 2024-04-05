# buddy_mpi
add_library(buddy_mpi SHARED buddy_mpi.cpp host_buffer.cpp dpu_conn.cpp sockets.cpp rdma.cpp dma.cpp request.cpp)
target_link_libraries(buddy_mpi PRIVATE MPI::MPI_C -libverbs -ldoca_common -ldoca_dma -labsl_hash -labsl_raw_hash_set)

target_link_directories(buddy_mpi PRIVATE /opt/mellanox/doca/lib64 /opt/mellanox/doca/lib/aarch64-linux-gnu)
target_include_directories(buddy_mpi PRIVATE /opt/mellanox/doca/include)
target_compile_options(buddy_mpi PRIVATE -D DOCA_ALLOW_EXPERIMENTAL_API)

# buddy_proxy
add_library(buddy_proxy SHARED sockets.cpp rdma.cpp proxy.cpp dma.cpp)
target_link_libraries(buddy_proxy PRIVATE -libverbs -ldoca_common -ldoca_dma)

target_link_directories(buddy_proxy PRIVATE /opt/mellanox/doca/lib64 /opt/mellanox/doca/lib/aarch64-linux-gnu)
target_include_directories(buddy_proxy PRIVATE /opt/mellanox/doca/include)
target_compile_options(buddy_proxy PRIVATE -D DOCA_ALLOW_EXPERIMENTAL_API)
