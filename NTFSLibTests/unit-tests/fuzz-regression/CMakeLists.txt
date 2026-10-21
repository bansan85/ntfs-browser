target_sources(
  NtfsBrowserTests
  PRIVATE
    "bad-cluster-fuzz-inputs.cpp"
    "fuzzer-regression-tests.cpp"
    "index-slack-fuzz-inputs.cpp"
    "record-slack-fuzz-inputs.cpp"
    "usn-fuzz-inputs.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
