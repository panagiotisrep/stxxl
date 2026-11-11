# Injected into this repository's STXXL top-level CMake project (stxxl) by build.sh via
# -DCMAKE_PROJECT_stxxl_INCLUDE=<this file>. STXXL must be the top-level project (it uses
# CMAKE_SOURCE_DIR throughout), and its include/library variables are only set later in its CMakeLists,
# so the driver targets are added once STXXL's top-level CMakeLists has been processed (cmake_language DEFER).
# STXXL's own CMake files are not modified; the drivers get the same flags as boa.
cmake_minimum_required(VERSION 3.19) # cmake_language(DEFER)

# Compile-time block sizes (template arguments). Defaults as in examples/containers/map1.cpp and
# unordered_map1.cpp.
set(STXXL_MAP_NODE_BLOCK_SIZE 32768 CACHE STRING "stxxl::map node block size in bytes")
set(STXXL_MAP_LEAF_BLOCK_SIZE 32768 CACHE STRING "stxxl::map leaf block size in bytes")
set(STXXL_UM_SUB_BLOCK_SIZE 8192 CACHE STRING "stxxl::unordered_map sub-block size in bytes")
set(STXXL_UM_SUB_BLOCKS_PER_BLOCK 256 CACHE STRING "stxxl::unordered_map sub-blocks per block")

set(BOA_STXXL_COMPETITOR_DIR ${CMAKE_CURRENT_LIST_DIR})

function(boa_add_stxxl_competitors)
  foreach(structure map unordered_map)
    set(target stxxl_${structure}_bench)
    add_executable(${target} ${BOA_STXXL_COMPETITOR_DIR}/stxxl_bench.cpp)
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    string(TOUPPER ${structure} upper)
    target_compile_definitions(${target} PRIVATE STXXL_COMPETITOR_${upper}
      STXXL_MAP_NODE_BLOCK_SIZE=${STXXL_MAP_NODE_BLOCK_SIZE}
      STXXL_MAP_LEAF_BLOCK_SIZE=${STXXL_MAP_LEAF_BLOCK_SIZE}
      STXXL_UM_SUB_BLOCK_SIZE=${STXXL_UM_SUB_BLOCK_SIZE}
      STXXL_UM_SUB_BLOCKS_PER_BLOCK=${STXXL_UM_SUB_BLOCKS_PER_BLOCK})
    # Same as STXXL's stxxl_build_tool (used for boa), plus common.h.
    target_include_directories(${target} PRIVATE ${STXXL_INCLUDE_DIRS} ${BOA_STXXL_COMPETITOR_DIR}/..)
    target_link_libraries(${target} PRIVATE ${STXXL_LIBRARIES})
  endforeach()
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL boa_add_stxxl_competitors)
