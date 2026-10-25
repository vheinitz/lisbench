cmake_minimum_required(VERSION 3.25)
project(lisbench VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LISBENCH_BUILD_TESTS "Build the unit tests" ON)
option(LISBENCH_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
option(LISBENCH_WERROR "Treat warnings as errors" ON)
set(LISBENCH_SANITIZE "" CACHE STRING "Sanitizers, e.g. address,undefined or thread")

include(cmake/deps.cmake)

add_library(lisbench_warnings INTERFACE)
if(MSVC)
  target_compile_options(lisbench_warnings INTERFACE /W4 /permissive- /utf-8 $<$<BOOL:${LISBENCH_WERROR}>:/WX>)
else()
  target_compile_options(lisbench_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual
    -Wimplicit-fallthrough
    $<$<BOOL:${LISBENCH_WERROR}>:-Werror>)
  # GCC 13/14 report these from inside libstdc++ after inlining at -O2/-O3
  # (vector and string internals), not from this code. They stay visible as
  # warnings but do not fail the build.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(lisbench_warnings INTERFACE
      -Wno-error=stringop-overflow -Wno-error=stringop-overread
      -Wno-error=free-nonheap-object -Wno-error=array-bounds)
  endif()
endif()

if(LISBENCH_SANITIZE)
  add_compile_options(-fsanitize=${LISBENCH_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${LISBENCH_SANITIZE})
endif()

add_library(lisbench_core
  src/lis01_frame.cpp
  src/lis01_decoder.cpp
  src/lis01_link.cpp
  src/lis02_codec.cpp
  src/lis02_message.cpp
  src/lis02_records.cpp
  src/printable.cpp
  src/watch.cpp
)
add_library(lisbench::core ALIAS lisbench_core)
target_include_directories(lisbench_core PUBLIC include)
target_link_libraries(lisbench_core PRIVATE lisbench_warnings)

add_library(lisbench_io
  src/io/endpoint.cpp
  src/io/faults.cpp
  src/io/relay.cpp
  src/io/session.cpp
)
add_library(lisbench::io ALIAS lisbench_io)
target_link_libraries(lisbench_io PUBLIC lisbench::core asio PRIVATE lisbench_warnings)

add_library(lisbench_sim
  src/sim/scenario.cpp
  src/sim/runner.cpp
)
add_library(lisbench::sim ALIAS lisbench_sim)
target_link_libraries(lisbench_sim PUBLIC lisbench::io PRIVATE lisbench_warnings yaml-cpp::yaml-cpp)

add_library(lisbench_mapper
  src/mapper/template.cpp
  src/mapper/sqlite.cpp
  src/mapper/mapping.cpp
)
add_library(lisbench::mapper ALIAS lisbench_mapper)
target_link_libraries(lisbench_mapper PUBLIC lisbench::core sqlite3 PRIVATE lisbench_warnings yaml-cpp::yaml-cpp)

add_library(lisbench_monitor
  src/monitor/monitor.cpp
  src/monitor/capture.cpp
)
add_library(lisbench::monitor ALIAS lisbench_monitor)
target_link_libraries(lisbench_monitor PUBLIC lisbench::io PRIVATE lisbench_warnings nlohmann_json::nlohmann_json)

add_executable(lisbench apps/lisbench/main.cpp apps/lisbench/cmd_monitor.cpp apps/lisbench/cmd_bridge.cpp
  apps/lisbench/cmd_map.cpp)
target_compile_definitions(lisbench PRIVATE LISBENCH_VERSION="${PROJECT_VERSION}")
target_link_libraries(lisbench PRIVATE lisbench::core lisbench::sim lisbench::monitor lisbench::mapper lisbench_warnings CLI11::CLI11 nlohmann_json::nlohmann_json)

if(LISBENCH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LISBENCH_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()
