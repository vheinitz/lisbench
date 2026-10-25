add_executable(lisbench_tests
  test_lis01_frame.cpp
  test_lis01_link.cpp
  test_lis02_codec.cpp
  test_lis02_message.cpp
  test_corpus.cpp
  test_printable.cpp
  test_robustness.cpp
)
target_link_libraries(lisbench_tests PRIVATE lisbench::core lisbench_warnings Catch2::Catch2WithMain)
target_compile_definitions(lisbench_tests PRIVATE LISBENCH_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")

include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(lisbench_tests)
