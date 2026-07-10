cmake_minimum_required(VERSION 3.21)
project(eda-lab CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Pull in odb + utl only. OpenROAD's own CMakeLists builds a lot more than
# we want; add_subdirectory the whole thing but only link the targets we need —
# do NOT try to build the top-level `openroad` target.
add_subdirectory(third_party/openroad EXCLUDE_FROM_ALL)

add_executable(hello_odb src/dbio/hello_odb.cpp)
target_link_libraries(hello_odb PRIVATE odb utl)

add_library(hypergraph src/hypergraph/hypergraph.cpp)
target_include_directories(hypergraph PUBLIC src)
target_link_libraries(hypergraph PUBLIC odb utl)

# Inert definitions of OpenROAD-application globals referenced by utl.a's
# swig/Tcl wrapper objects; see src/support/ord_shim.cpp.
add_library(ord_shim src/support/ord_shim.cpp)

add_library(netlistgen src/engines/netlistgen/netlistgen.cpp)
target_include_directories(netlistgen PUBLIC src)
target_link_libraries(netlistgen PUBLIC odb utl ord_shim)

add_library(partitioning
  src/engines/partitioning/random_hypergraph.cpp
  src/engines/partitioning/fm_partitioner.cpp)
target_include_directories(partitioning PUBLIC src)
target_link_libraries(partitioning PUBLIC hypergraph odb utl ord_shim)

enable_testing()
find_package(GTest REQUIRED)

add_executable(hypergraph_test test/hypergraph_test.cpp)
target_compile_definitions(hypergraph_test
                           PRIVATE EDA_LAB_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
target_link_libraries(hypergraph_test PRIVATE hypergraph odb utl
                                              GTest::gtest GTest::gtest_main)

add_executable(netlistgen_test test/netlistgen_test.cpp)
target_link_libraries(netlistgen_test PRIVATE netlistgen hypergraph odb utl
                                              GTest::gtest GTest::gtest_main)

add_executable(netlistgen_stageb_test test/netlistgen_stageb_test.cpp)
target_compile_definitions(netlistgen_stageb_test
                           PRIVATE EDA_LAB_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
target_link_libraries(netlistgen_stageb_test PRIVATE netlistgen hypergraph odb
                                                     utl GTest::gtest
                                                     GTest::gtest_main)

# Library-linkage smoke test: builds an external consumer that links the
# `netlistgen` library target (PRIVATE netlistgen odb utl) and calls into it,
# WITHOUT compiling netlistgen's sources into the binary. If netlistgen ever
# regresses to header-only or its library target breaks, this fails to link.
add_executable(netlistgen_link_smoke test/netlistgen_link_smoke.cpp)
target_link_libraries(netlistgen_link_smoke PRIVATE netlistgen odb utl)

add_executable(fm_partitioner_test test/fm_partitioner_test.cpp)
target_compile_definitions(fm_partitioner_test
                           PRIVATE EDA_LAB_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
target_link_libraries(fm_partitioner_test PRIVATE partitioning hypergraph odb
                                                  utl GTest::gtest
                                                  GTest::gtest_main)

include(GoogleTest)
add_test(NAME hypergraph_test COMMAND hypergraph_test)
add_test(NAME netlistgen_test COMMAND netlistgen_test)
add_test(NAME netlistgen_stageb_test COMMAND netlistgen_stageb_test)
add_test(NAME netlistgen_link_smoke COMMAND netlistgen_link_smoke)
add_test(NAME fm_partitioner_test COMMAND fm_partitioner_test)