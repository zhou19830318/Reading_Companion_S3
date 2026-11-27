add_executable(BookSummaryCodecTest
  BookSummaryCodecTest.cpp
  ${REPO_ROOT}/lib/BookSummary/BookSummaryCodec.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(BookSummaryCodecTest PRIVATE
  ${REPO_ROOT}/lib/BookSummary
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookSummaryCodecTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookSummaryCodecTest)
