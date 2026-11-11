# Competitor benchmarks. Built separately from the main stxxl build, one build directory per competitor,
# each by its own script: <competitor>/build.sh → -DCOMPETITOR=<competitor> → <competitor>/CMakeLists.txt
cmake_minimum_required(VERSION 3.13)
project(boa_competitors CXX)

set(COMPETITOR "" CACHE STRING "Competitor to build: leveldb, pachash, rocksdb or silt")
if(NOT COMPETITOR MATCHES "^(leveldb|pachash|rocksdb|silt)$")
  message(FATAL_ERROR "Set -DCOMPETITOR=leveldb|pachash|rocksdb|silt (or use <competitor>/build.sh)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(COMPETITORS_DIR ${CMAKE_CURRENT_SOURCE_DIR}) # common.h
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}) # <build dir>/<competitor>_bench

add_subdirectory(${COMPETITOR})
