cmake_minimum_required(VERSION 3.25)
project(lisbench VERSION 0.1.0 LANGUAGES CXX)

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
  src/lis01_link.cpp
  src/lis02_codec.cpp
  src/lis02_message.cpp
  src/lis02_records.cpp
  src/printable.cpp
)
add_library(lisbench::core ALIAS lisbench_core)
target_include_directories(lisbench_core PUBLIC include)
target_link_libraries(lisbench_core PRIVATE lisbench_warnings)

add_executable(lisbench apps/lisbench/main.cpp)
target_link_libraries(lisbench PRIVATE lisbench::core lisbench_warnings CLI11::CLI11 nlohmann_json::nlohmann_json)

if(LISBENCH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LISBENCH_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()
