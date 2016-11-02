CMAKE_MINIMUM_REQUIRED(VERSION 2.8)

PROJECT(MICA)


SET(CMAKE_CXX_COMPILER g++-5)
SET(CMAKE_C_LINK_EXECUTABLE g++-5)



OPTION(DEBUG "Enable debugging" OFF)
OPTION(LTO "Use link time optimization" OFF)

# OPTION(AVX2 "Enable AVX2" ON)

OPTION(DPDK "Enable Intel DPDK" ON)



INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/src)
SET(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)
SET(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)
SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)



ADD_DEFINITIONS(-std=c++14)

ADD_DEFINITIONS(-Wall -Wextra)
ADD_DEFINITIONS(-Wzero-as-null-pointer-constant)
ADD_DEFINITIONS(-Wsign-conversion)
ADD_DEFINITIONS(-Wconversion)
ADD_DEFINITIONS(-Winline)

ADD_DEFINITIONS(-Wno-unused-function)

ADD_DEFINITIONS(-march=native)
ADD_DEFINITIONS(-pthread)

ADD_DEFINITIONS(-g)



IF(DEBUG)

  MESSAGE(WARNING "Debugging is ENABLED (to disable, run `cmake .. -DDEBUG=OFF`). Performance will be low.")
  ADD_DEFINITIONS(-Og)

ELSE(DEBUG)

  MESSAGE(STATUS "Debugging is disabled (to enable, run `cmake .. -DDEBUG=ON`)")
  ADD_DEFINITIONS(-DNDEBUG)
  ADD_DEFINITIONS(-O3)
  # ADD_DEFINITIONS(--param inline-unit-growth=200 --param large-function-growth=500)

ENDIF(DEBUG)


IF(LTO)

  MESSAGE(STATUS "Link time optimization is enabled (to disable, run `cmake .. -DLTO=OFF`)")
  ADD_DEFINITIONS(-flto)
  SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto")

ELSE(LTO)

  MESSAGE(WARNING "Link time optimization is DISABLED (to enable, run `cmake .. -DLTO=ON`). Performance will be low.")

ENDIF(LTO)


# IF(AVX2)

#   MESSAGE(STATUS "AVX2 is enabled (to disable, run `cmake .. -DAVX2=OFF`)")
#   ADD_DEFINITIONS(-mavx2)
#   SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -mavx2")

# ELSE(LTO)

#   MESSAGE(WARNING "AVX2 is DISABLED (to enable, run `cmake .. -DAVX2=ON`). Performance will be low.")

# ENDIF(AVX2)


IF(DPDK)

  MESSAGE(STATUS "Intel DPDK support is enabled (to disable, run `cmake .. -DDPDK=OFF`)")
  set(RTE_SDK ${CMAKE_SOURCE_DIR}/build/dpdk)
  add_definitions(-DUSE_DPDK)
  add_definitions(-isystem ${RTE_SDK}/build/include -include rte_config.h)
  link_directories(${LINK_DIRECTORIES} ${RTE_SDK}/build/lib)
  set(LIBRARIES ${LIBRARIES} -Wl,--whole-archive)
  set(LIBRARIES ${LIBRARIES} rte_kvargs rte_mbuf ethdev rte_malloc rte_mempool rte_ring rte_eal)
  set(LIBRARIES ${LIBRARIES} rte_pmd_i40e rte_pmd_ixgbe rte_pmd_ring)
  set(LIBRARIES ${LIBRARIES} -Wl,--no-whole-archive)
  set(LIBRARIES ${LIBRARIES} rt dl)

ELSE(DPDK)

  MESSAGE(STATUS "Intel DPDK support is disabled (to enable, run `cmake .. -DDPDK=ON`)")

ENDIF(DPDK)



SET(SOURCES ${SOURCES} src/mica/alloc/hugetlbfs_shm.cc)
SET(SOURCES ${SOURCES} src/mica/directory/directory_client.cc)
SET(SOURCES ${SOURCES} src/mica/directory/etcdcpp/etcd_mod.cc)
SET(SOURCES ${SOURCES} src/mica/transaction/timestamp.cc)
SET(SOURCES ${SOURCES} src/mica/util/config.cc)
SET(SOURCES ${SOURCES} src/mica/util/cityhash/city_mod.cc)
SET(SOURCES ${SOURCES} src/mica/util/lcore.cc)
SET(SOURCES ${SOURCES} src/mica/util/stopwatch.cc)
SET(SOURCES ${SOURCES} src/mica/util/zipf.cc)

SET(LIBRARIES ${LIBRARIES} rt numa pthread)
SET(LIBRARIES ${LIBRARIES} curl)
# SET(LIBRARIES ${LIBRARIES} boost_coroutine boost_system)



IF(LTO)

  ADD_EXECUTABLE(test_table src/mica/test/test_table.cc ${SOURCES})
  TARGET_LINK_LIBRARIES(test_table ${LIBRARIES})

  ADD_EXECUTABLE(test_trie src/mica/test/test_trie.cc ${SOURCES})
  TARGET_LINK_LIBRARIES(test_trie ${LIBRARIES})

  ADD_EXECUTABLE(test_processor src/mica/test/test_processor.cc ${SOURCES})
  TARGET_LINK_LIBRARIES(test_processor ${LIBRARIES})

  ADD_EXECUTABLE(test_load src/mica/test/test_load.cc ${SOURCES})
  TARGET_LINK_LIBRARIES(test_load ${LIBRARIES})

  ADD_EXECUTABLE(test_zipf src/mica/test/test_zipf.cc ${SOURCES})
  TARGET_LINK_LIBRARIES(test_zipf ${LIBRARIES})

  ADD_EXECUTABLE(test_prefetch src/mica/test/test_prefetch.cc ${SOURCES})
  TARGET_LINK_LIBRARIES(test_prefetch ${LIBRARIES})

  ADD_EXECUTABLE(test_rand src/mica/test/test_rand.cc ${SOURCES})
  TARGET_LINK_LIBRARIES(test_rand ${LIBRARIES})

  ADD_EXECUTABLE(test_atomics src/mica/test/test_atomics.cc ${SOURCES})
  TARGET_LINK_LIBRARIES(test_atomics ${LIBRARIES})

  ADD_EXECUTABLE(test_tsc_sync src/mica/test/test_tsc_sync.cc ${SOURCES})
  TARGET_LINK_LIBRARIES(test_tsc_sync ${LIBRARIES})

  ADD_EXECUTABLE(test_tx src/mica/test/test_tx.cc ${SOURCES})
  TARGET_LINK_LIBRARIES(test_tx ${LIBRARIES})

  ADD_EXECUTABLE(microbench src/mica/test/microbench.cc ${SOURCES})
  TARGET_LINK_LIBRARIES(microbench ${LIBRARIES})

  IF(DPDK)

    ADD_EXECUTABLE(server src/mica/test/server.cc ${SOURCES})
    TARGET_LINK_LIBRARIES(server ${LIBRARIES})

    ADD_EXECUTABLE(netbench src/mica/test/netbench.cc ${SOURCES})
    TARGET_LINK_LIBRARIES(netbench ${LIBRARIES})

  ENDIF(DPDK)

ELSE(LTO)

  ADD_LIBRARY(common ${SOURCES})

  ADD_EXECUTABLE(test_table src/mica/test/test_table.cc)
  TARGET_LINK_LIBRARIES(test_table common ${LIBRARIES})

  ADD_EXECUTABLE(test_trie src/mica/test/test_trie.cc)
  TARGET_LINK_LIBRARIES(test_trie common ${LIBRARIES})

  ADD_EXECUTABLE(test_processor src/mica/test/test_processor.cc)
  TARGET_LINK_LIBRARIES(test_processor common ${LIBRARIES})

  ADD_EXECUTABLE(test_load src/mica/test/test_load.cc)
  TARGET_LINK_LIBRARIES(test_load common ${LIBRARIES})

  ADD_EXECUTABLE(test_zipf src/mica/test/test_zipf.cc)
  TARGET_LINK_LIBRARIES(test_zipf common ${LIBRARIES})

  ADD_EXECUTABLE(test_prefetch src/mica/test/test_prefetch.cc)
  TARGET_LINK_LIBRARIES(test_prefetch common ${LIBRARIES})

  ADD_EXECUTABLE(test_rand src/mica/test/test_rand.cc ${SOURCES})
  TARGET_LINK_LIBRARIES(test_rand ${LIBRARIES})

  ADD_EXECUTABLE(test_atomics src/mica/test/test_atomics.cc ${SOURCES})
  TARGET_LINK_LIBRARIES(test_atomics ${LIBRARIES})

  ADD_EXECUTABLE(test_tsc_sync src/mica/test/test_tsc_sync.cc ${SOURCES})
  TARGET_LINK_LIBRARIES(test_tsc_sync ${LIBRARIES})

  ADD_EXECUTABLE(test_tx src/mica/test/test_tx.cc ${SOURCES})
  TARGET_LINK_LIBRARIES(test_tx ${LIBRARIES})

  ADD_EXECUTABLE(microbench src/mica/test/microbench.cc)
  TARGET_LINK_LIBRARIES(microbench common ${LIBRARIES})

  IF(DPDK)

    ADD_EXECUTABLE(server src/mica/test/server.cc)
    TARGET_LINK_LIBRARIES(server common ${LIBRARIES})

    ADD_EXECUTABLE(netbench src/mica/test/netbench.cc)
    TARGET_LINK_LIBRARIES(netbench common ${LIBRARIES})

  ENDIF(DPDK)

ENDIF(LTO)

