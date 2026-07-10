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
target_include_directories(hello_odb PRIVATE src)
target_link_libraries(hello_odb PRIVATE odb utl)

add_library(hypergraph src/hypergraph/hypergraph.cpp)
target_include_directories(hypergraph PUBLIC src)
target_link_libraries(hypergraph PUBLIC odb utl)

# Inert definitions of OpenROAD-application globals referenced by utl.a's
# swig/Tcl wrapper objects; see src/support/ord_shim.cpp.
add_library(ord_shim src/support/ord_shim.cpp)

add_library(netlistgen
  src/engines/netlistgen/netlistgen.cpp
  src/engines/netlistgen/netlist_writers.cpp
  src/engines/netlistgen/netlist_validation.cpp)
target_include_directories(netlistgen PUBLIC src)
target_link_libraries(netlistgen PUBLIC odb utl ord_shim)

# nlohmann::json — header-only JSON parser for the standalone CLI's config
# input only (never linked into the netlistgen library). Pulled via
# FetchContent pinned to a tagged release from GitHub (an allowed domain).
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# CLI config layer: JSON -> SyntheticNetlistSpec + validate-before-write policy.
# Kept separate from the netlistgen library so JSON never leaks into the
# in-memory API; linked into both the CLI executable and the CLI tests.
add_library(netlistgen_cli_core src/engines/netlistgen/cli_config.cpp)
target_include_directories(netlistgen_cli_core PUBLIC src)
target_link_libraries(netlistgen_cli_core
                      PUBLIC netlistgen odb utl
                      PRIVATE nlohmann_json::nlohmann_json)

add_executable(netlistgen_cli src/engines/netlistgen/netlistgen_cli.cpp)
target_link_libraries(netlistgen_cli PRIVATE netlistgen_cli_core)

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

add_executable(netlistgen_stagec_test test/netlistgen_stagec_test.cpp)
target_compile_definitions(netlistgen_stagec_test
                           PRIVATE EDA_LAB_DATA_DIR="${CMAKE_SOURCE_DIR}/data"
                                   NETLISTGEN_CLI_BIN="$<TARGET_FILE:netlistgen_cli>")
target_link_libraries(netlistgen_stagec_test PRIVATE netlistgen_cli_core
                                                     netlistgen odb utl
                                                     GTest::gtest
                                                     GTest::gtest_main)
add_dependencies(netlistgen_stagec_test netlistgen_cli)

add_executable(fm_partitioner_test test/fm_partitioner_test.cpp)
target_compile_definitions(fm_partitioner_test
                           PRIVATE EDA_LAB_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
target_link_libraries(fm_partitioner_test PRIVATE partitioning hypergraph odb
                                                  utl GTest::gtest
                                                  GTest::gtest_main)

# CLI --help / usage convention: unit-tests support/cli.h and spawns both CLI
# binaries to confirm --help (exit 0) and missing-argument (exit nonzero) share
# one option-description source. Header-only helper — no odb/utl link needed.
add_executable(cli_help_test test/cli_help_test.cpp)
target_include_directories(cli_help_test PRIVATE src)
target_compile_definitions(cli_help_test
                           PRIVATE HELLO_ODB_BIN="$<TARGET_FILE:hello_odb>"
                                   NETLISTGEN_CLI_BIN="$<TARGET_FILE:netlistgen_cli>")
target_link_libraries(cli_help_test PRIVATE GTest::gtest GTest::gtest_main)
add_dependencies(cli_help_test hello_odb netlistgen_cli)

include(GoogleTest)
add_test(NAME hypergraph_test COMMAND hypergraph_test)
add_test(NAME netlistgen_test COMMAND netlistgen_test)
add_test(NAME netlistgen_stageb_test COMMAND netlistgen_stageb_test)
add_test(NAME netlistgen_link_smoke COMMAND netlistgen_link_smoke)
add_test(NAME netlistgen_stagec_test COMMAND netlistgen_stagec_test)
add_test(NAME fm_partitioner_test COMMAND fm_partitioner_test)
add_test(NAME cli_help_test COMMAND cli_help_test)