# ---------------------------------------------------------------- LMDB (extern/lmdb, release LMDB_0.9.36)
# LMDB's Makefile is not used: its three C files (mdb.c, midl.c) are compiled here with CMake's flags.
enable_language(C)
set(LMDB_DIR ${REPO_ROOT}/extern/lmdb/libraries/liblmdb)
add_library(lmdb STATIC ${LMDB_DIR}/mdb.c ${LMDB_DIR}/midl.c)
target_include_directories(lmdb PUBLIC ${LMDB_DIR})
target_compile_options(lmdb PRIVATE -O2 -fPIC -w)
target_link_libraries(lmdb PRIVATE pthread)

add_executable(lmdb_bench lmdb_bench.cpp)
target_include_directories(lmdb_bench PRIVATE ${REPO_ROOT}/include ${COMPETITORS_DIR})
target_link_libraries(lmdb_bench PRIVATE lmdb)
