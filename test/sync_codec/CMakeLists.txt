add_executable(SyncCodecTest
  SyncCodecTest.cpp
  ${REPO_ROOT}/lib/Sync/SyncCodec.cpp
  ${REPO_ROOT}/lib/ReaderQuery/ReaderQueryCodec.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(SyncCodecTest PRIVATE
  ${REPO_ROOT}/lib/Sync
  ${REPO_ROOT}/lib/ReaderQuery
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(SyncCodecTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SyncCodecTest)
