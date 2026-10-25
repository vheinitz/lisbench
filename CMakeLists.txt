cmake_minimum_required(VERSION 3.25)
project(lisbench VERSION 0.7.1 LANGUAGES C CXX)

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
  # C4996 marks standard getenv/strerror as "unsafe" in favour of the
  # Microsoft-only _s variants; the code stays with the standard functions.
  target_compile_definitions(lisbench_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
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
  src/io/command.cpp
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
  src/sim/profile.cpp
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
  src/monitor/trigger.cpp
)
add_library(lisbench::monitor ALIAS lisbench_monitor)
target_link_libraries(lisbench_monitor PUBLIC lisbench::io PRIVATE lisbench_warnings nlohmann_json::nlohmann_json)

add_library(lisbench_web
  src/web/http.cpp
  src/web/server.cpp
)
add_library(lisbench::web ALIAS lisbench_web)
target_link_libraries(lisbench_web PUBLIC lisbench::core asio PRIVATE lisbench_warnings)

add_library(lisbench_conform
  src/conform/suite.cpp
  src/conform/tester.cpp
)
add_library(lisbench::conform ALIAS lisbench_conform)
target_link_libraries(lisbench_conform PUBLIC lisbench::io PRIVATE lisbench_warnings)

add_library(lisbench_webapp
  src/web/app.cpp
)
add_library(lisbench::webapp ALIAS lisbench_webapp)
target_link_libraries(lisbench_webapp PUBLIC lisbench::web PRIVATE lisbench::monitor lisbench::sim
  lisbench::conform lisbench_warnings nlohmann_json::nlohmann_json)

set(LISBENCH_WEB_FILES index.html style.css app.js)
list(TRANSFORM LISBENCH_WEB_FILES PREPEND "${PROJECT_SOURCE_DIR}/web/" OUTPUT_VARIABLE LISBENCH_WEB_PATHS)
add_custom_command(
  OUTPUT ${PROJECT_BINARY_DIR}/generated/web_assets.cpp
  COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}/web "-DFILES=${LISBENCH_WEB_FILES}"
          -DOUT=${PROJECT_BINARY_DIR}/generated/web_assets.cpp -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${LISBENCH_WEB_PATHS} ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
  COMMENT "Embedding the web UI"
  VERBATIM)
add_library(lisbench_webassets ${PROJECT_BINARY_DIR}/generated/web_assets.cpp)
target_include_directories(lisbench_webassets PUBLIC include)
target_compile_features(lisbench_webassets PUBLIC cxx_std_23)

add_executable(lisbench apps/lisbench/main.cpp apps/lisbench/cmd_monitor.cpp apps/lisbench/cmd_bridge.cpp
  apps/lisbench/cmd_map.cpp apps/lisbench/cmd_conform.cpp apps/lisbench/cmd_serve.cpp)
# Where --profile finds profiles by id: the source tree for a development
# build, /usr/share/lisbench/devices in the package.
set(LISBENCH_DEVICES_DIR "${PROJECT_SOURCE_DIR}/docs/devices" CACHE PATH
    "Directory with the instrument profiles that --profile looks up by id")
target_compile_definitions(lisbench PRIVATE LISBENCH_VERSION="${PROJECT_VERSION}"
                                           LISBENCH_DEVICES_DIR="${LISBENCH_DEVICES_DIR}")
target_link_libraries(lisbench PRIVATE lisbench::core lisbench::sim lisbench::monitor lisbench::mapper lisbench::conform lisbench::web lisbench::webapp lisbench_webassets lisbench_warnings CLI11::CLI11 nlohmann_json::nlohmann_json)

if(LISBENCH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LISBENCH_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

include(cmake/packaging.cmake)
