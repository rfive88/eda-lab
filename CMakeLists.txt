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