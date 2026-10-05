include(GoogleTest)

add_executable(lob_tests
    test_object_pool.cpp
    test_bitset_index.cpp
    test_order_index.cpp
    test_book_side.cpp
    test_engine.cpp
    test_itch.cpp
    test_differential.cpp)

target_link_libraries(lob_tests PRIVATE lob::itch GTest::gtest GTest::gtest_main)
lob_set_warnings(lob_tests)

gtest_discover_tests(lob_tests)
