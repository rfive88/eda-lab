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

enable_testing()
find_package(GTest REQUIRED)

add_executable(hypergraph_test test/hypergraph_test.cpp)
target_compile_definitions(hypergraph_test
                           PRIVATE EDA_LAB_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
target_link_libraries(hypergraph_test PRIVATE hypergraph odb utl
                                              GTest::gtest GTest::gtest_main)

include(GoogleTest)
add_test(NAME hypergraph_test COMMAND hypergraph_test)