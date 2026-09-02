add_library(leaper_core
  src/gbdt.cc
  src/collector.cc
  src/overlap.cc
  src/predictor.cc
  src/leaper.cc
)
target_include_directories(leaper_core PUBLIC include PRIVATE src)

add_executable(gbdt_check src/gbdt_check.cc)
target_include_directories(gbdt_check PRIVATE src include)
target_link_libraries(gbdt_check PRIVATE leaper_core)

add_executable(mapper_check src/mapper_check.cc)
target_include_directories(mapper_check PRIVATE src include)
target_link_libraries(mapper_check PRIVATE leaper_core)

add_executable(collector_bench src/collector_bench.cc)
target_include_directories(collector_bench PRIVATE src include)
target_link_libraries(collector_bench PRIVATE leaper_core)
find_package(Threads REQUIRED)
target_link_libraries(collector_bench PRIVATE Threads::Threads)
