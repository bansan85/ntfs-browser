target_sources(
  NtfsBrowserTests
  PRIVATE
    "coverage-mft-tree-tests.cpp"
    "file-reference-tests.cpp"
    "fragmented-mft-invalid-record-tests.cpp"
    "mft-addr-narrowing-tests.cpp"
    "mft-tree-bad-state-tests.cpp"
    "mft-tree-bitmap-tests.cpp"
    "mft-tree-data-state-batch-tests.cpp"
    "mft-tree-data-state-tests.cpp"
    "mft-slot-reader-tests.cpp"
    "mft-tree-bounded-cache-tests.cpp"
    "mft-extent-map-tests.cpp"
    "mft-tree-malformed-record-tests.cpp"
    "mft-tree-tests.cpp"
    "ntfsmftlist-sample-tests.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)

# NtfsMftList runs on Windows and Linux (examples/CMakeLists.txt).
add_dependencies(NtfsBrowserTests NtfsMftList)
target_compile_definitions(
  NtfsBrowserTests
  PRIVATE NTFS_MFT_LIST_EXE="$<TARGET_FILE:NtfsMftList>"
)
