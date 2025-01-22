add_executable(ygm-trace ygm-trace.cpp)
target_link_libraries(ygm-trace PRIVATE buddy absl::flat_hash_map)

add_executable(histo histo.cpp)
target_link_libraries(histo PRIVATE buddy)
target_compile_definitions(histo PRIVATE MPP_RAW_MPI=1)

add_executable(route_inl_serial route.cpp)
target_link_libraries(route_inl_serial PRIVATE buddy)
target_compile_definitions(route_inl_serial PRIVATE INLINE=1)

add_executable(route_sep_serial route.cpp)
target_link_libraries(route_sep_serial PRIVATE buddy)

if (OpenMP_CXX_FOUND)
    add_executable(route_sep_atomic route.cpp)
    target_link_libraries(route_sep_atomic PRIVATE buddy OpenMP::OpenMP_CXX)
    target_compile_definitions(route_sep_atomic PRIVATE ATOMIC_OUT=1)

    add_executable(route_sep_iso route.cpp)
    target_link_libraries(route_sep_iso PRIVATE buddy OpenMP::OpenMP_CXX)
    target_compile_definitions(route_sep_iso PRIVATE SEPARATE_OUT=1)
endif()

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
