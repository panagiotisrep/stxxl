# Injected into LeanStore's own top-level CMake project (leanstoredb) by build.sh via
# -DCMAKE_PROJECT_leanstoredb_INCLUDE=<this file>. LeanStore must be the top-level project (it uses
# CMAKE_SOURCE_DIR throughout), so instead of add_subdirectory(LeanStore), this adds the driver target to
# LeanStore's build. LeanStore itself is built unmodified, with its own flags and pinned dependencies.
set(BOA_COMPETITORS_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
set(BOA_REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}) # <build dir>/leanstore_bench

add_executable(leanstore_bench ${CMAKE_CURRENT_LIST_DIR}/leanstore_bench.cpp)
target_include_directories(leanstore_bench PRIVATE ${BOA_REPO_ROOT}/include ${BOA_COMPETITORS_DIR})
# Created before LeanStore sets CMAKE_CXX_STANDARD 20, so set it on the target.
set_target_properties(leanstore_bench PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
# The target "leanstore" (and its dependencies gflags, tbb, aio, ...) is defined later by LeanStore's
# CMakeLists; target names are resolved when the build system is generated.
target_link_libraries(leanstore_bench PRIVATE leanstore Threads::Threads)
