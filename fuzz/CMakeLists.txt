# libFuzzer targets; configure with Clang and -DLISBENCH_BUILD_FUZZERS=ON.
foreach(target fuzz_deframe fuzz_lis02_parse fuzz_link)
  add_executable(${target} ${target}.cpp)
  target_link_libraries(${target} PRIVATE lisbench::core)
  target_compile_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
endforeach()
