add_library(buddy_mpi SHARED buddy_mpi.cpp host_buffer.cpp dpu_conn.cpp sockets.cpp rdma.cpp)
target_link_libraries(buddy_mpi PRIVATE MPI::MPI_C -libverbs)

add_library(buddy_proxy SHARED sockets.cpp rdma.cpp)
target_link_libraries(buddy_proxy PRIVATE -libverbs)
