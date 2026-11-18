# ---------------------------------------------------------------- WiredTiger (extern/wiredtiger, tag mongodb-9.0.8)
# WiredTiger's CMake project is not usable through add_subdirectory (it includes files relative to the top-level
# source directory), so build.sh builds it on its own into <build dir>/wiredtiger-lib (static library, no
# compressors, no Python API) and passes that folder here as -DWIREDTIGER_LIB_DIR.
set(WIREDTIGER_LIB_DIR "" CACHE PATH "Build directory of extern/wiredtiger (set by wiredtiger/build.sh)")
find_library(WIREDTIGER_LIBRARY NAMES wiredtiger PATHS ${WIREDTIGER_LIB_DIR} NO_DEFAULT_PATH PATH_SUFFIXES . lib REQUIRED)
find_path(WIREDTIGER_INCLUDE_DIR wiredtiger.h PATHS ${WIREDTIGER_LIB_DIR}/include NO_DEFAULT_PATH REQUIRED)

add_executable(wiredtiger_bench wiredtiger_bench.cpp)
target_include_directories(wiredtiger_bench PRIVATE ${REPO_ROOT}/include ${COMPETITORS_DIR} ${WIREDTIGER_INCLUDE_DIR})
target_link_libraries(wiredtiger_bench PRIVATE ${WIREDTIGER_LIBRARY} pthread dl)
