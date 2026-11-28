add_executable(BookLibraryCodecTest
  BookLibraryCodecTest.cpp
  ${REPO_ROOT}/lib/BookLibrary/BookLibraryCodec.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(BookLibraryCodecTest PRIVATE
  ${REPO_ROOT}/lib/BookLibrary
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookLibraryCodecTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookLibraryCodecTest)
