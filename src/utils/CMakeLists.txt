set(RDMA_UTILS_SRC
  StringHelper.h
  Filehelper.h
  Config.h
  Logging.h
  Logging.cc
  Config.cc
  Timer.h
  Network.h
  CpuNumaUtils.h
  GpuNumaUtils.h
  RdmaCounter.h
  ) # Adding headers required for portability reasons http://voices.canonical.com/jussi.pakkanen/2013/03/26/a-list-of-common-cmake-antipatterns/
add_library(rdma_utils ${RDMA_UTILS_SRC})

find_package(Numa)
target_link_libraries(rdma_utils ${NUMA_LIBRARY})

target_include_directories(rdma_utils PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
set_target_properties(rdma_utils PROPERTIES OUTPUT_NAME "rdma-manager-utils")
target_link_libraries(rdma_utils rdma_proto)

install(TARGETS rdma_utils
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
