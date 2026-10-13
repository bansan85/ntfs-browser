target_sources(
  NtfsBrowserTests
  PRIVATE
    "coverage-mft-tree-tests.cpp"
    "fragmented-mft-invalid-record-tests.cpp"
    "mft-addr-narrowing-tests.cpp"
    "mft-extent-map-tests.cpp"
    "mft-tree-tests.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
