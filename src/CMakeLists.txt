add_library(buddy_mpi SHARED buddy_mpi.cpp host_buffer.cpp)
target_link_libraries(buddy_mpi PRIVATE MPI::MPI_C)
