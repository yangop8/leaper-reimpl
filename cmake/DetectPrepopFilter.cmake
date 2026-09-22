# Does the RocksDB at ROCKSDB_INCLUDE carry the prepopulate-filter patch
# (adapters/rocksdb/rocksdb-11.8-prepopulate-filter.patch)? Sets
# LEAPER_HAVE_PREPOP_FILTER. The probe is re-run on every configure: the
# result must follow the header, not the CMake cache, because the patch is
# applied after a first configure in the documented setup order (M9 review
# follow-up, 2026-09-22, F1). One tiny compile per configure is the cost.
include(CheckCXXSourceCompiles)
unset(LEAPER_HAVE_PREPOP_FILTER CACHE)
unset(LEAPER_HAVE_PREPOP_FILTER)
set(CMAKE_REQUIRED_INCLUDES "${ROCKSDB_INCLUDE}")
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
  message(STATUS "RocksDB prepopulate filter patch: present (warm_mode=prepop available)")
else()
  message(STATUS "RocksDB prepopulate filter patch: absent (warm_mode=prepop unavailable; iterator/sst only)")
endif()
