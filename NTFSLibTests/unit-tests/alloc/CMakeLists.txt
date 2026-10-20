target_sources(
  NtfsBrowserTests
  PRIVATE
    "bitmap-failure-tests.cpp"
    "bitmap-memory-tests.cpp"
    "cluster-bitmap-tests.cpp"
    "data-state-fold-tests.cpp"
    "file-record-data-state-tests.cpp"
    "live-file-check-tests.cpp"
    "read-only-use-tests.cpp"
    "space-summary-tests.cpp"
    "volume-cluster-state-tests.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
