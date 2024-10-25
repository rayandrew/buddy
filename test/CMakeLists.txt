if(NOT MPI_FOUND)
	return()
endif()

add_executable(ygm-trace ygm-trace.cpp)
target_link_libraries(ygm-trace PRIVATE buddy absl::flat_hash_map)

add_executable(histo histo.cpp)
target_link_libraries(histo PRIVATE buddy)
target_compile_definitions(histo PRIVATE MPP_RAW_MPI=1)

return()

foreach(case hello around-the-world write_barrier iter double_buf mpi_any)
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
