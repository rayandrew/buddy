if (absl_FOUND)
    add_executable(ygm-trace ygm-trace.cpp)
    target_link_libraries(ygm-trace PRIVATE buddy absl::flat_hash_map)
    # Dont want to fix this now
    target_compile_options(ygm-trace PRIVATE -Wno-narrowing)
endif()

add_executable(histo histo.cpp)
target_link_libraries(histo PRIVATE buddy)
target_compile_definitions(histo PRIVATE MPP_RAW_MPI=1)

add_executable(buddy-lat buddy-lat.cpp)
target_link_libraries(buddy-lat PRIVATE buddy)

add_executable(route-base route.cpp)
target_link_libraries(route-base PRIVATE buddy OpenMP::OpenMP_CXX)

add_executable(route-fixed route.cpp)
target_link_libraries(route-fixed PRIVATE buddy OpenMP::OpenMP_CXX)
target_compile_definitions(route-fixed PRIVATE FIXED=1)

add_executable(route-fixed-term route.cpp)
target_link_libraries(route-fixed-term PRIVATE buddy OpenMP::OpenMP_CXX)
target_compile_definitions(route-fixed-term PRIVATE FIXED=1 TERM=1)

add_executable(route-fixed-term-simd route.cpp)
target_link_libraries(route-fixed-term-simd PRIVATE buddy OpenMP::OpenMP_CXX)
target_compile_definitions(route-fixed-term-simd PRIVATE FIXED=1 TERM=1 SIMD=1)

# if (OpenMP_CXX_FOUND)
#     add_executable(route_sep_atomic route.cpp)
#     target_link_libraries(route_sep_atomic PRIVATE buddy OpenMP::OpenMP_CXX)
#     target_compile_definitions(route_sep_atomic PRIVATE ATOMIC_OUT=1)

    # add_executable(route_sep_iso route.cpp)
    # target_link_libraries(route_sep_iso PRIVATE buddy OpenMP::OpenMP_CXX)
    # target_compile_definitions(route_sep_iso PRIVATE SEPARATE_OUT=1)
# endif()

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
