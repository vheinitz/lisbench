add_executable(lisbench_tests
  test_lis01_frame.cpp
  test_lis01_link.cpp
  test_lis02_codec.cpp
  test_lis02_message.cpp
  test_corpus.cpp
  test_printable.cpp
  test_robustness.cpp
  test_io.cpp
  test_scenario.cpp
  test_sim_e2e.cpp
  test_serial_e2e.cpp
  test_lis01_decoder.cpp
  test_watch.cpp
  test_monitor.cpp
  test_monitor_e2e.cpp
  test_bridge_e2e.cpp
  test_mapper.cpp
  test_devsim.cpp
)
target_link_libraries(lisbench_tests PRIVATE lisbench::core lisbench::sim lisbench::monitor lisbench::mapper lisbench_warnings Catch2::Catch2WithMain)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(lisbench_tests PRIVATE util)
endif()
target_compile_definitions(lisbench_tests PRIVATE LISBENCH_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus"
                                                  LISBENCH_DEVICES_DIR="${PROJECT_SOURCE_DIR}/docs/devices")

include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(lisbench_tests)
