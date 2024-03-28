add_executable(hello_mpi hello.cpp)
target_link_libraries(hello_mpi PRIVATE MPI::MPI_C)

add_executable(hello_buddy hello.cpp)
target_link_libraries(hello_buddy PRIVATE buddy_mpi MPI::MPI_C)

add_executable(buddy-proxy proxy_main.cpp)
target_link_libraries(buddy-proxy PRIVATE buddy_proxy)
