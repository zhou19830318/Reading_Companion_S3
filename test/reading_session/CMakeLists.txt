add_executable(ReadingSessionFormatTest
  ReadingSessionFormatTest.cpp
  ReadingSessionTrackerTest.cpp
  ReadingAggregateFormatTest.cpp
)

target_include_directories(ReadingSessionFormatTest PRIVATE
  ${REPO_ROOT}/lib/ReadingHistory
  ${REPO_ROOT}/lib/ChatLog
  ${REPO_ROOT}/lib/Utf8
)

target_sources(ReadingSessionFormatTest PRIVATE
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_link_libraries(ReadingSessionFormatTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingSessionFormatTest)
