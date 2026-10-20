target_sources(
  NtfsBrowserTests
  PRIVATE
    "usn-benchmark-tests.cpp"
    "usn-cap-tests.cpp"
    "usn-failure-cache-tests.cpp"
    "usn-journal-load-tests.cpp"
    "usn-mft-tree-tests.cpp"
    "usn-record-scanner-tests.cpp"
    "usn-resync-tests.cpp"
    "usn-sparse-read-tests.cpp"
    "usn-strict-mode-tests.cpp"
    "usn-test-support.h"
)

# alloc-failure-support.h, shared with the allocation tests, captures log lines.
target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../alloc"
)
