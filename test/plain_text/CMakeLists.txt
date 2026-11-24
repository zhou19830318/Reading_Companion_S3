add_executable(PlainTextTest
  PlainTextTest.cpp
  ${REPO_ROOT}/lib/PlainText/PlainText.cpp
)

target_include_directories(PlainTextTest PRIVATE
  ${REPO_ROOT}/lib/PlainText
)

target_link_libraries(PlainTextTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PlainTextTest)
