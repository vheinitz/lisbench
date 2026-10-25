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
  test_trigger.cpp
  test_profile.cpp
  test_conform.cpp
  test_web.cpp
  test_webapp.cpp
)
target_link_libraries(lisbench_tests PRIVATE lisbench::core lisbench::sim lisbench::monitor lisbench::mapper lisbench::conform lisbench::web lisbench::webapp lisbench_warnings nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(lisbench_tests PRIVATE util)
endif()
target_compile_definitions(lisbench_tests PRIVATE LISBENCH_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus"
                                                  LISBENCH_DEVICES_DIR="${PROJECT_SOURCE_DIR}/docs/devices")

include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
# PRE_TEST: a cross build cannot run the test binary while it is being built.
catch_discover_tests(lisbench_tests DISCOVERY_MODE PRE_TEST)
