target_sources(
  NtfsBrowserTests
  PRIVATE "bad-cluster-fuzz-inputs.cpp" "fuzzer-regression-tests.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
