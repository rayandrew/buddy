add_library(buddy_mpi SHARED buddy_mpi.cpp host_buffer.cpp dpu_conn.cpp sockets.cpp)
target_link_libraries(buddy_mpi PRIVATE MPI::MPI_C)

add_library(buddy_proxy SHARED sockets.cpp)
