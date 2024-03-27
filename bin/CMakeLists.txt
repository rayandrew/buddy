add_executable(hello_mpi hello.cpp)
add_executable(hello_buddy hello.cpp)
target_link_libraries(hello_mpi PRIVATE MPI::MPI_C)
target_link_libraries(hello_buddy PRIVATE buddy_mpi MPI::MPI_C)
