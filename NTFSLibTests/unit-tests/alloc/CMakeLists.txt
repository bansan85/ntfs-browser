target_sources(
  NtfsBrowserTests
  PRIVATE
    "bad-cluster-ceiling-tests.cpp"
    "bad-cluster-data-state-tests.cpp"
    "bad-cluster-failure-tests.cpp"
    "bad-cluster-list-tests.cpp"
    "bad-cluster-live-check-tests.cpp"
    "bad-cluster-state-tests.cpp"
    "bad-cluster-summary-tests.cpp"
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
