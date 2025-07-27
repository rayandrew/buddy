add_executable(triangle triangle.cpp std_options.cpp spmat_io.cpp spmat_utils.cpp triangle_buddy.cpp)
target_link_libraries(triangle PRIVATE buddy)
target_include_directories(triangle PRIVATE .)

add_executable(transpose_matrix transpose_matrix.cpp std_options.cpp spmat_io.cpp spmat_utils.cpp spmat_buddy.cpp)
target_link_libraries(transpose_matrix PRIVATE buddy)
target_include_directories(transpose_matrix PRIVATE .)

#add_executable(permute_matrix permute_matrix.cpp std_options.cpp spmat_io.cpp spmat_utils.cpp spmat_buddy.cpp)
#target_link_libraries(permute_matrix PRIVATE buddy)
#target_include_directories(permute_matrix PRIVATE .)