# ---------------------------------------------------------------- PaCHash (extern/pachash, commit f7f18fd)
cmake_minimum_required(VERSION 3.25) # required by PaCHash

# PaCHash's block size (StoreConfig::BLOCK_LENGTH) is a compile-time constant. 4096 = PaCHash's default.
set(PACHASH_BLOCK_LENGTH 4096 CACHE STRING "PaCHash block size in bytes (VARIABLE_SIZE_STORE_BLOCK_LENGTH)")
# PaCHash uses io_uring (construction read-back and the 'uring' query I/O) only if liburing happens to be
# installed, and Linux AIO via libaio likewise. Both are made explicit here instead:
#   PACHASH_WITH_URING=OFF  never use io_uring (default; same binary on every machine)
#   PACHASH_WITH_URING=ON   require liburing (configuration fails if it is missing)
#   libaio                  never used (the driver does not offer PaCHash's PosixAIO)
option(PACHASH_WITH_URING "Build PaCHash with io_uring (requires liburing)" OFF)
set(CMAKE_DISABLE_FIND_PACKAGE_LibAIO TRUE)
if(PACHASH_WITH_URING)
  list(APPEND CMAKE_MODULE_PATH ${REPO_ROOT}/extern/pachash/cmake)
  find_package(LibUring REQUIRED)
else()
  set(CMAKE_DISABLE_FIND_PACKAGE_LibUring TRUE)
endif()

# Not top level, so PaCHash adds neither -march=native nor its examples/benchmarks.
add_subdirectory(${REPO_ROOT}/extern/pachash ${CMAKE_BINARY_DIR}/extern-pachash EXCLUDE_FROM_ALL)

add_executable(pachash_bench pachash_bench.cpp)
target_include_directories(pachash_bench PRIVATE ${REPO_ROOT}/include ${COMPETITORS_DIR})
target_compile_definitions(pachash_bench PRIVATE VARIABLE_SIZE_STORE_BLOCK_LENGTH=${PACHASH_BLOCK_LENGTH})
target_link_libraries(pachash_bench PRIVATE PaCHash::pachash) # C++20 (required by PaCHash)
