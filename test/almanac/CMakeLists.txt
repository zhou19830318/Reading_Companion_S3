add_executable(AlmanacTest
  AlmanacTest.cpp
  ${REPO_ROOT}/lib/Almanac/Almanac.cpp
)

target_include_directories(AlmanacTest PRIVATE
  ${REPO_ROOT}/lib/Almanac
)

target_link_libraries(AlmanacTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AlmanacTest)
