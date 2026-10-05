add_executable(lob_bench bench_book.cpp)
target_link_libraries(lob_bench PRIVATE lob::itch benchmark::benchmark)
lob_set_warnings(lob_bench)
lob_tune(lob_bench)
