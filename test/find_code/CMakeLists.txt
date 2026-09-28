add_executable(FindCodeTest
  FindCodeTest.cpp
  FindStateTest.cpp
  FindJournalTest.cpp
  ${REPO_ROOT}/lib/FindMode/FindCode.cpp
  ${REPO_ROOT}/lib/FindMode/FindState.cpp
  ${REPO_ROOT}/lib/FindMode/FindJournal.cpp
)

target_include_directories(FindCodeTest PRIVATE ${REPO_ROOT}/lib/Serialization)

target_link_libraries(FindCodeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FindCodeTest)
