add_library(common INTERFACE)
target_link_libraries(common INTERFACE buddy)
target_include_directories(common INTERFACE .)
target_sources(common INTERFACE std_options.cpp spmat_io.cpp spmat_utils.cpp buddy_convey.cpp)

add_executable(triangle triangle.cpp triangle_buddy.cpp)
target_link_libraries(triangle PRIVATE common)

add_executable(transpose_matrix transpose_matrix.cpp std_options.cpp spmat_io.cpp spmat_utils.cpp spmat_buddy.cpp)
target_link_libraries(transpose_matrix PRIVATE common)

add_executable(ig ig.cpp ig_buddy.cpp std_options.cpp spmat_io.cpp spmat_utils.cpp spmat_buddy.cpp)
target_link_libraries(ig PRIVATE common)

#add_executable(permute_matrix permute_matrix.cpp std_options.cpp spmat_io.cpp spmat_utils.cpp spmat_buddy.cpp)
#target_link_libraries(permute_matrix PRIVATE buddy)
#target_include_directories(permute_matrix PRIVATE .)
