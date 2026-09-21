cmake_minimum_required(VERSION 3.16)
project(leaper VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Vendored LevelDB, pinned to release 1.23 (see third_party/leveldb).
set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LEVELDB_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/leveldb EXCLUDE_FROM_ALL)

# LevelDB is compiled with -fno-rtti, so no typeinfo is emitted for
# leveldb::Cache or leveldb::Logger. We subclass both out of tree (StatsCache,
# EventLogger), so anything deriving from them must match that setting or the
# link fails on "typeinfo for leveldb::Cache". Neither Leaper nor the bench
# uses dynamic_cast/typeid, so matching is free.
if(MSVC)
  set(LEAPER_NO_RTTI /GR-)
else()
  set(LEAPER_NO_RTTI -fno-rtti)
endif()

add_subdirectory(leaper)

add_library(leaper_leveldb adapters/leveldb/leaper_leveldb.cc)
target_include_directories(leaper_leveldb PUBLIC adapters/leveldb)
target_link_libraries(leaper_leveldb PUBLIC leaper_core leveldb)
target_compile_options(leaper_leveldb PRIVATE ${LEAPER_NO_RTTI})

add_executable(mixgraph_check bench/src/mixgraph_check.cc)
target_include_directories(mixgraph_check PRIVATE bench/include)

add_executable(leaper_bench bench/src/leaper_bench.cc)
target_include_directories(leaper_bench PRIVATE bench/include leaper/include)
target_compile_options(leaper_bench PRIVATE ${LEAPER_NO_RTTI})
target_link_libraries(leaper_bench PRIVATE leaper_leveldb leveldb)
find_package(Threads REQUIRED)
target_link_libraries(leaper_bench PRIVATE Threads::Threads)

# --- RocksDB (M5-M7) -------------------------------------------------------
# Built out of tree because a full RocksDB build inside this project would
# dominate configure time for anyone who only wants the LevelDB half:
#   cmake -S third_party/rocksdb -B build-rocksdb -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DWITH_GFLAGS=0 -DWITH_TESTS=0 -DWITH_TOOLS=0 -DROCKSDB_BUILD_SHARED=0
#   cmake --build build-rocksdb --target rocksdb -j
set(ROCKSDB_LIB "${CMAKE_SOURCE_DIR}/build-rocksdb/librocksdb.a")
if(EXISTS "${ROCKSDB_LIB}")
  add_library(rocksdb_static STATIC IMPORTED)
  set_target_properties(rocksdb_static PROPERTIES
    IMPORTED_LOCATION "${ROCKSDB_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/third_party/rocksdb/include")

  add_library(leaper_rocksdb adapters/rocksdb/leaper_rocksdb.cc)
  target_include_directories(leaper_rocksdb PUBLIC adapters/rocksdb)
  # The selective-prepopulate path needs the 43-line RocksDB patch
  # (adapters/rocksdb/rocksdb-11.8-prepopulate-filter.patch). Detect it, so
  # that the adapter and the bench still build against a pristine 11.8 with
  # the iterator and sst warm modes only (M9 review, 2026-09-21, R2).
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_INCLUDES "${CMAKE_SOURCE_DIR}/third_party/rocksdb/include")
  set(CMAKE_REQUIRED_FLAGS "-std=c++17")
  check_cxx_source_compiles("
    #include \"rocksdb/table.h\"
    struct F : rocksdb::PrepopulateBlockFilter {
      bool ShouldWarm(rocksdb::TableFileCreationReason, const rocksdb::Slice&,
                      const rocksdb::Slice&) override { return true; }
    };
    int main() { rocksdb::BlockBasedTableOptions o; o.prepopulate_block_filter = nullptr; return 0; }
  " LEAPER_HAVE_PREPOP_FILTER)
  unset(CMAKE_REQUIRED_INCLUDES)
  unset(CMAKE_REQUIRED_FLAGS)
  if(LEAPER_HAVE_PREPOP_FILTER)
    target_compile_definitions(leaper_rocksdb PUBLIC LEAPER_HAVE_PREPOP_FILTER=1)
    message(STATUS "RocksDB prepopulate filter patch: present (warm_mode=prepop available)")
  else()
    message(STATUS "RocksDB prepopulate filter patch: absent (warm_mode=prepop unavailable; iterator/sst only)")
  endif()
  # RocksDB is also compiled -fno-rtti (its USE_RTTI defaults off), so anything
  # deriving from rocksdb::EventListener has to match or the link fails on
  # "typeinfo for rocksdb::Customizable". leaper_core keeps RTTI on so that the
  # RTTI-enabled world can still derive from its interfaces.
  target_compile_options(leaper_rocksdb PRIVATE ${LEAPER_NO_RTTI})
  target_link_libraries(leaper_rocksdb PUBLIC leaper_core rocksdb_static)

  add_executable(leaper_bench_rocksdb bench/src/leaper_bench_rocksdb.cc)
  target_include_directories(leaper_bench_rocksdb PRIVATE bench/include leaper/include)
  target_compile_options(leaper_bench_rocksdb PRIVATE ${LEAPER_NO_RTTI})
  target_link_libraries(leaper_bench_rocksdb PRIVATE leaper_rocksdb Threads::Threads)
  if(APPLE)
    target_link_libraries(leaper_bench_rocksdb PRIVATE "-framework CoreFoundation" "-framework Security")
  endif()

  # The per-job warm budget on a file where the predicted ranges have no keys.
  add_executable(budget_check adapters/rocksdb/budget_check.cc)
  target_include_directories(budget_check PRIVATE leaper/include adapters/rocksdb)
  target_compile_options(budget_check PRIVATE ${LEAPER_NO_RTTI})
  target_link_libraries(budget_check PRIVATE leaper_rocksdb Threads::Threads)
  if(APPLE)
    target_link_libraries(budget_check PRIVATE "-framework CoreFoundation" "-framework Security")
  endif()

  # Feasibility check for block-level warming through SstFileReader.
  add_executable(sst_warm_check adapters/rocksdb/sst_warm_check.cc)
  target_compile_options(sst_warm_check PRIVATE ${LEAPER_NO_RTTI})
  target_link_libraries(sst_warm_check PRIVATE rocksdb_static Threads::Threads)
  if(APPLE)
    target_link_libraries(sst_warm_check PRIVATE "-framework CoreFoundation" "-framework Security")
  endif()
else()
  message(STATUS "RocksDB not built (${ROCKSDB_LIB} missing); skipping M5-M7 targets")
endif()
