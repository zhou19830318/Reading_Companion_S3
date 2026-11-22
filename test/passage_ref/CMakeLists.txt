add_executable(PassageRefTest
  PassageRefTest.cpp
)

target_include_directories(PassageRefTest PRIVATE
  ${REPO_ROOT}/lib/Epub
)

target_compile_options(PassageRefTest PRIVATE -Wno-format-truncation)

target_link_libraries(PassageRefTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PassageRefTest)
