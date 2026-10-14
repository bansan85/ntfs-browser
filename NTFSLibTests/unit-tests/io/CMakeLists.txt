target_sources(
  NtfsBrowserTests
  PRIVATE
    "disk-read-failure-tests.cpp"
    "disk-reader-tests.cpp"
    "file-reader-tests.cpp"
    "record-offset-overflow-tests.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)

# NtfsDir and NtfsDir2 are Windows-only samples (examples/CMakeLists.txt), so
# their test exists only where the samples build.
if(WIN32)
  target_sources(NtfsBrowserTests PRIVATE "ntfsdir-sample-tests.cpp")
  add_dependencies(NtfsBrowserTests NtfsDir NtfsDir2)
  target_compile_definitions(
    NtfsBrowserTests
    PRIVATE
      NTFS_DIR_EXE="$<TARGET_FILE:NtfsDir>"
      NTFS_DIR2_EXE="$<TARGET_FILE:NtfsDir2>"
  )

  # NtfsUndel's search and recovery logic (examples/ntfsundel/ntfsundel-core.h)
  # is Windows-only, like its dialog. The tests link it directly, so the
  # dialog's MFC never enters the test binary.
  target_sources(
    NtfsBrowserTests
    PRIVATE "ntfsundel-recover-tests.cpp" "ntfsundel-search-tests.cpp"
  )
  target_link_libraries(NtfsBrowserTests PRIVATE NtfsUndelCore)

  # NtfsDump's and NtfsAttr's dump logic (examples/ntfsdump/ntfsdump-core.h,
  # examples/ntfsattr/ntfsattr-core.h) are Windows-only, like their dialogs,
  # and link the same way.
  target_sources(
    NtfsBrowserTests
    PRIVATE "ntfsdump-core-tests.cpp" "ntfsattr-core-tests.cpp"
  )
  target_link_libraries(NtfsBrowserTests PRIVATE NtfsDumpCore NtfsAttrCore)
endif()
