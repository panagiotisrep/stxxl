# ---------------------------------------------------------------- RocksDB (extern/rocksdb, release v11.13.2)
set(CMAKE_CXX_STANDARD 20) # required by RocksDB 11

# Only the static library: no tests, tools, benchmarks, shared library or Java bindings.
set(WITH_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_ALL_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK OFF CACHE BOOL "" FORCE)
set(WITH_CORE_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_TRACE_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_JNI OFF CACHE BOOL "" FORCE)
set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
# Same binary on every machine (RocksDB's defaults would depend on the build machine):
#   PORTABLE=1             baseline CPU instructions; RocksDB's default 0 adds -march=native (BOA is not built with it)
#   WITH_LIBURING=OFF      RocksDB's default ON uses io_uring only if liburing happens to be installed
#   WITH_GFLAGS=OFF        only needed for RocksDB's tools
#   FAIL_ON_WARNINGS=OFF   newer compilers than RocksDB's CI must not break the build
# Compression (WITH_SNAPPY/LZ4/ZLIB/ZSTD/BZ2), jemalloc, NUMA and folly stay at RocksDB's default OFF:
# no compression (BOA stores data uncompressed too), system malloc.
set(PORTABLE 1 CACHE STRING "" FORCE)
set(WITH_LIBURING OFF CACHE BOOL "" FORCE)
set(WITH_GFLAGS OFF CACHE BOOL "" FORCE)
set(FAIL_ON_WARNINGS OFF CACHE BOOL "" FORCE)
add_subdirectory(${REPO_ROOT}/extern/rocksdb ${CMAKE_BINARY_DIR}/extern-rocksdb EXCLUDE_FROM_ALL)

add_executable(rocksdb_bench rocksdb_bench.cpp)
target_include_directories(rocksdb_bench PRIVATE ${REPO_ROOT}/include ${COMPETITORS_DIR}
                           ${REPO_ROOT}/extern/rocksdb/include)
target_link_libraries(rocksdb_bench PRIVATE rocksdb)
