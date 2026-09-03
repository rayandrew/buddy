set(common_src sockets.cpp rdma.cpp request.cpp)
if(LOCAL_DMA)
	set(common_src ${common_src} dma.cpp)
endif()
if(DOCA_FABRIC)
	set(common_src ${common_src} rdma_doca.cpp)
endif()
if(DPA_FABRIC)
	set(common_src ${common_src} rdma_dpa.cpp)
endif()

add_library(buddy_common INTERFACE)
target_link_libraries(buddy_common INTERFACE -libverbs)

if(LOCAL_DMA)
	target_link_directories(buddy_common INTERFACE /opt/mellanox/doca/lib64 /opt/mellanox/doca/lib/${CMAKE_LIBRARY_ARCHITECTURE} /opt/mellanox/doca/lib/x86_64-linux-gnu /opt/mellanox/doca/lib/aarch64-linux-gnu)
	target_include_directories(buddy_common INTERFACE /opt/mellanox/doca/include)
	target_compile_options(buddy_common INTERFACE -D DOCA_ALLOW_EXPERIMENTAL_API)
	target_link_libraries(buddy_common INTERFACE -ldoca_common -ldoca_dma)
endif()

if(DOCA_FABRIC)
	target_link_directories(buddy_common INTERFACE /opt/mellanox/doca/lib/${CMAKE_LIBRARY_ARCHITECTURE} /opt/mellanox/doca/lib/aarch64-linux-gnu)
	target_include_directories(buddy_common INTERFACE /opt/mellanox/doca/include)
	target_compile_options(buddy_common INTERFACE -DDOCA_ALLOW_EXPERIMENTAL_API -DBUDDY_FABRIC)
	target_link_libraries(buddy_common INTERFACE -ldoca_common -ldoca_rdma)
endif()

# The DPA kernel is compiled by dpacc into a static archive holding the device image; --app-name
# must match the buddy_dpa_app symbol rdma_dpa.cpp declares.
if(DPA_FABRIC)
	set(DPA_ARCHIVE ${CMAKE_CURRENT_BINARY_DIR}/buddy_dpa_app.a)
	add_custom_command(OUTPUT ${DPA_ARCHIVE}
		COMMAND /opt/mellanox/doca/tools/dpacc ${CMAKE_CURRENT_SOURCE_DIR}/rdma_dpa_dev.c
			${CMAKE_CURRENT_SOURCE_DIR}/dpa_echo_dev.c
			${CMAKE_CURRENT_SOURCE_DIR}/dpa_d2d_dev.c
			-o ${DPA_ARCHIVE} -mcpu=nv-dpa-bf3 -hostcc=gcc
			-hostcc-options=-DFLEXIO_ALLOW_EXPERIMENTAL_API
			--devicecc-options=-DFLEXIO_DEV_ALLOW_EXPERIMENTAL_API,-O2,-Wall
			--app-name=buddy_dpa_app
			-device-libs=-L/opt/mellanox/doca/lib/aarch64-linux-gnu,-ldoca_dpa_dev,-ldoca_dpa_dev_comm
			-flto -I/opt/mellanox/doca/include
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/rdma_dpa_dev.c ${CMAKE_CURRENT_SOURCE_DIR}/dpa_echo_dev.c
			${CMAKE_CURRENT_SOURCE_DIR}/dpa_d2d_dev.c
		COMMENT "dpacc buddy_dpa_app")
	add_custom_target(buddy_dpa_kernel DEPENDS ${DPA_ARCHIVE})
	target_link_directories(buddy_common INTERFACE /opt/mellanox/doca/lib/${CMAKE_LIBRARY_ARCHITECTURE} /opt/mellanox/doca/lib/aarch64-linux-gnu /opt/mellanox/flexio/lib)
	target_include_directories(buddy_common INTERFACE /opt/mellanox/doca/include)
	target_compile_options(buddy_common INTERFACE -DDOCA_ALLOW_EXPERIMENTAL_API -DBUDDY_FABRIC -DFABRIC_DPA)
	target_link_libraries(buddy_common INTERFACE ${DPA_ARCHIVE} -ldoca_common -ldoca_rdma -ldoca_dpa -lflexio -lmlx5)
endif()

#if(MPI_FOUND)
#add_library(buddy_mpi SHARED ${common_src} buddy_mpi.cpp host_buffer.cpp dpu_conn.cpp)
#target_link_libraries(buddy_mpi PRIVATE MPI::MPI_C buddy_common absl::flat_hash_map)
#endif()

add_library(buddy SHARED ${common_src} host_api.cpp dpu_conn.cpp hostbufs.cpp)
target_link_libraries(buddy PRIVATE buddy_common)
target_link_libraries(buddy PUBLIC MPI::MPI_C)

if(DPA_FABRIC)
	add_executable(dpa-echo ${common_src} dpa_echo.cpp)
	add_dependencies(dpa-echo buddy_dpa_kernel)
	target_link_libraries(dpa-echo PRIVATE buddy_common)

	add_executable(dpa-d2d ${common_src} dpa_d2d.cpp dpa_d2d_test.cpp)
	add_dependencies(dpa-d2d buddy_dpa_kernel)
	target_link_libraries(dpa-d2d PRIVATE buddy_common)
endif()

if(DOCA_FABRIC OR DPA_FABRIC)
	add_executable(fabric-lat ${common_src} fabric_lat.cpp)
	if(DPA_FABRIC)
		add_dependencies(fabric-lat buddy_dpa_kernel)
	endif()
	target_link_libraries(fabric-lat PRIVATE buddy_common)
endif()

add_executable(buddy-proxy ${common_src} proxy_main.cpp proxy.cpp)
if(DPA_FABRIC)
	add_dependencies(buddy-proxy buddy_dpa_kernel)
endif()
target_link_libraries(buddy-proxy PRIVATE buddy_common)
target_link_libraries(buddy-proxy PRIVATE MPI::MPI_C)
if(OpenMP_CXX_FOUND)
	target_link_libraries(buddy-proxy PRIVATE OpenMP::OpenMP_CXX)
endif()
