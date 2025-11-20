include(ExternalProject)

# This breaks if they contain spaces!! Could not find a way to fix it.
# Something with them being treated as multiple arguments interacting with quoatation.
get_property(CXXFLAGS TARGET hpctoolkit PROPERTY INTERFACE_COMPILE_OPTIONS)
get_property(LDFLAGS TARGET hpctoolkit PROPERTY INTERFACE_LINK_OPTIONS)
get_property(LDLIBS TARGET hpctoolkit PROPERTY INTERFACE_LINK_LIBRARIES)

ExternalProject_Add(qs
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/qs
  CONFIGURE_COMMAND ""
  #CONFIGURE_COMMAND rm -rf ${CMAKE_CURRENT_BINARY_DIR}/qs && cp -r ${CMAKE_CURRENT_SOURCE_DIR}/qs ${CMAKE_CURRENT_BINARY_DIR}/qs
  BUILD_COMMAND
  rsync -a ${CMAKE_CURRENT_SOURCE_DIR}/qs ${CMAKE_CURRENT_BINARY_DIR} &&
  cmake -E env
    "CXX=${MPI_CXX_COMPILER}"
     "CXXFLAGS=${CXXFLAGS}"
     "LDFLAGS=${LDFLAGS} ${LDLIBS}"
    "BUDDY_SRC=${CMAKE_SOURCE_DIR}"
    "BUDDY_BUILD=${CMAKE_BINARY_DIR}"
    make -C "${CMAKE_CURRENT_BINARY_DIR}/qs/src"
  INSTALL_COMMAND ""
  BUILD_ALWAYS 1
)
