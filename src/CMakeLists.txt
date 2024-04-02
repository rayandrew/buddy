add_library(buddy_mpi SHARED buddy_mpi.cpp host_buffer.cpp dpu_conn.cpp sockets.cpp rdma.cpp)
target_link_libraries(buddy_mpi PRIVATE MPI::MPI_C -libverbs)

add_library(buddy_proxy SHARED sockets.cpp rdma.cpp proxy.cpp)
target_link_libraries(buddy_proxy PRIVATE -libverbs -ldoca_common)

target_link_directories(buddy_proxy PRIVATE /opt/mellanox/doca/lib64 /opt/mellanox/doca/lib/aarch64-linux-gnu)
target_include_directories(buddy_proxy PRIVATE /opt/mellanox/doca/include)
target_compile_options(buddy_proxy PRIVATE -D DOCA_ALLOW_EXPERIMENTAL_API)
