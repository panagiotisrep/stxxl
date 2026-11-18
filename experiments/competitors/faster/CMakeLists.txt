# ---------------------------------------------------------------- FASTER (extern/faster, release v2.6.5, C++ library in cc/)
# FASTER's own CMake project downloads googletest at configure time, so its three library sources are compiled here.
# FASTER is C++14 and uses libaio, libuuid and TBB.
set(FASTER_CC ${REPO_ROOT}/extern/faster/cc/src)
add_library(faster STATIC ${FASTER_CC}/core/address.cc ${FASTER_CC}/core/lss_allocator.cc ${FASTER_CC}/core/thread.cc
            ${FASTER_CC}/environment/file_linux.cc)
target_include_directories(faster PUBLIC ${FASTER_CC})
target_compile_options(faster PRIVATE -w -O3 -std=c++17)
find_library(AIO_LIBRARY aio REQUIRED)
find_library(UUID_LIBRARY uuid REQUIRED)
find_library(TBB_LIBRARY tbb REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(faster PUBLIC ${AIO_LIBRARY} ${UUID_LIBRARY} ${TBB_LIBRARY} Threads::Threads)

add_executable(faster_bench faster_bench.cpp)
target_include_directories(faster_bench PRIVATE ${REPO_ROOT}/include ${COMPETITORS_DIR})
target_compile_options(faster_bench PRIVATE -w)
target_link_libraries(faster_bench PRIVATE faster)
