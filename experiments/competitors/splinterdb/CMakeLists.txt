# ---------------------------------------------------------------- SplinterDB (extern/splinterdb, commit 73f89d8, main)
# SplinterDB's Makefile is the only supported build, so build.sh builds libsplinterdb.a with it (release mode,
# out of tree) and passes that build folder here as -DSPLINTERDB_LIB_DIR.
set(SPLINTERDB_LIB_DIR "" CACHE PATH "Build output folder of extern/splinterdb (set by splinterdb/build.sh)")
find_library(SPLINTERDB_LIBRARY NAMES libsplinterdb.a PATHS ${SPLINTERDB_LIB_DIR}/lib NO_DEFAULT_PATH REQUIRED)
find_library(AIO_LIBRARY aio REQUIRED)
find_library(XXHASH_LIBRARY xxhash REQUIRED)

add_executable(splinterdb_bench splinterdb_bench.cpp)
target_compile_definitions(splinterdb_bench PRIVATE SPLINTERDB_PLATFORM_DIR=platform_linux _GNU_SOURCE)
target_include_directories(splinterdb_bench PRIVATE ${REPO_ROOT}/include ${COMPETITORS_DIR} ${REPO_ROOT}/extern/splinterdb/include)
target_link_libraries(splinterdb_bench PRIVATE ${SPLINTERDB_LIBRARY} ${AIO_LIBRARY} ${XXHASH_LIBRARY} pthread m)
