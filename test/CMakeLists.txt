if(NOT MPI_FOUND)
	return()
endif()

foreach(case hello around-the-world write_barrier iter double_buf)
    add_executable(${case}_mpi ${case}.cpp)
    target_link_libraries(${case}_mpi PRIVATE MPI::MPI_C)

    add_executable(${case}_buddy ${case}.cpp)
    target_link_libraries(${case}_buddy PRIVATE buddy_mpi MPI::MPI_C)
endforeach()

foreach(case halo3d)
    add_executable(${case}_mpi ember/mpi/${case}/${case}.c)
    target_link_libraries(${case}_mpi PRIVATE MPI::MPI_C)

    add_executable(${case}_buddy ember/mpi/${case}/${case}.c)
    target_link_libraries(${case}_buddy PRIVATE buddy_mpi MPI::MPI_C)
endforeach()
