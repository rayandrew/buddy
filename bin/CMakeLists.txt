# proxy
add_executable(buddy-proxy proxy_main.cpp)
target_link_libraries(buddy-proxy PRIVATE buddy_proxy)

#examples
add_executable(hello_mpi hello.cpp)
target_link_libraries(hello_mpi PRIVATE MPI::MPI_C)

add_executable(hello_buddy hello.cpp)
target_link_libraries(hello_buddy PRIVATE buddy_mpi MPI::MPI_C)

add_executable(around-the-world_mpi around-the-world.cpp)
target_link_libraries(around-the-world_mpi PRIVATE MPI::MPI_C)

add_executable(around-the-world_buddy around-the-world.cpp)
target_link_libraries(around-the-world_buddy PRIVATE buddy_mpi MPI::MPI_C)
