add_executable(ReadingPlanFormatTest
  ReadingPlanFormatTest.cpp
  ${REPO_ROOT}/lib/ReadingPlan/ReadingPlanFormat.cpp
)

target_include_directories(ReadingPlanFormatTest PRIVATE
  ${REPO_ROOT}/lib/ReadingPlan
)

target_link_libraries(ReadingPlanFormatTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingPlanFormatTest)

add_executable(ReadingAdviceTest
  ReadingAdviceTest.cpp
  ${REPO_ROOT}/lib/ReadingPlan/ReadingPlanFormat.cpp
  ${REPO_ROOT}/lib/ReadingPlan/ReadingAdvice.cpp
)

target_include_directories(ReadingAdviceTest PRIVATE
  ${REPO_ROOT}/lib/ReadingPlan
)

target_link_libraries(ReadingAdviceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingAdviceTest)
