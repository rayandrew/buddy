add_executable(triangle triangle.cpp std_options.cpp spmat_io.cpp spmat_utils.cpp triangle_buddy.cpp)
target_link_libraries(triangle PRIVATE buddy)
target_include_directories(triangle PRIVATE .)
