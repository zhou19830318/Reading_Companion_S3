add_executable(ReaderQueryCodecTest
  ReaderQueryCodecTest.cpp
  ${REPO_ROOT}/lib/ReaderQuery/ReaderQueryCodec.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(ReaderQueryCodecTest PRIVATE
  ${REPO_ROOT}/lib/ReaderQuery
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(ReaderQueryCodecTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderQueryCodecTest)
