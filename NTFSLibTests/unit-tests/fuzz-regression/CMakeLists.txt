target_sources(
  NtfsBrowserTests
  PRIVATE
    "alloc-scan-fuzz-inputs.cpp"
    "bad-cluster-fuzz-inputs.cpp"
    "fuzzer-regression-tests.cpp"
    "index-slack-fuzz-inputs.cpp"
    "mft-scan-fuzz-inputs.cpp"
    "record-slack-fuzz-inputs.cpp"
    "run-list-fuzz-inputs.cpp"
    "slack-names-fuzz-inputs.cpp"
    "usn-fuzz-inputs.cpp"
    "usn-tree-fuzz-inputs.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
