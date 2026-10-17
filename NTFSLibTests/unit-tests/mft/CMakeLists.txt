target_sources(
  NtfsBrowserTests
  PRIVATE
    "coverage-mft-tree-tests.cpp"
    "fragmented-mft-invalid-record-tests.cpp"
    "mft-addr-narrowing-tests.cpp"
    "mft-tree-bitmap-tests.cpp"
    "mft-slot-reader-tests.cpp"
    "mft-tree-bounded-cache-tests.cpp"
    "mft-extent-map-tests.cpp"
    "mft-tree-tests.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)

# NtfsMftList is a Windows-only sample (examples/CMakeLists.txt), so its test
# exists only where the sample builds.
if(WIN32)
  target_sources(NtfsBrowserTests PRIVATE "ntfsmftlist-sample-tests.cpp")
  add_dependencies(NtfsBrowserTests NtfsMftList)
  target_compile_definitions(
    NtfsBrowserTests
    PRIVATE NTFS_MFT_LIST_EXE="$<TARGET_FILE:NtfsMftList>"
  )
endif()
