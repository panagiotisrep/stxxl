# ---------------------------------------------------------------- LevelDB (extern/leveldb, release 1.23)
# Only the library: no tests, benchmarks or install rules.
set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LEVELDB_INSTALL OFF CACHE BOOL "" FORCE)
# LevelDB links Snappy, crc32c and tcmalloc if they happen to be installed, which would make the binary
# depend on the machine. Pre-setting the detection results turns all three off everywhere:
#   HAVE_SNAPPY=0    no compression (BOA stores data uncompressed too)
#   HAVE_CRC32C=0    LevelDB's portable CRC32C implementation
#   HAVE_TCMALLOC=0  system malloc, as for BOA
set(HAVE_SNAPPY 0 CACHE INTERNAL "")
set(HAVE_CRC32C 0 CACHE INTERNAL "")
set(HAVE_TCMALLOC 0 CACHE INTERNAL "")
add_subdirectory(${REPO_ROOT}/extern/leveldb ${CMAKE_BINARY_DIR}/extern-leveldb EXCLUDE_FROM_ALL)

add_executable(leveldb_bench leveldb_bench.cpp)
# workloads.h is header-only and uses only the standard library.
target_include_directories(leveldb_bench PRIVATE ${REPO_ROOT}/include ${COMPETITORS_DIR})
target_link_libraries(leveldb_bench PRIVATE leveldb)
