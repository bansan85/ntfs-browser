target_sources(
  NtfsBrowserTests
  PRIVATE
    "disk-read-failure-tests.cpp"
    "disk-reader-prefetch-tests.cpp"
    "disk-reader-size-tests.cpp"
    "disk-reader-tests.cpp"
    "extended-path-tests.cpp"
    "file-reader-tests.cpp"
    "file-record-extension-source-tests.cpp"
    "long-host-path-tests.cpp"
    "ntfsdir-sample-tests.cpp"
    "read-ahead-disk-reader-tests.cpp"
    "record-offset-overflow-tests.cpp"
    "traverse-standing-tests.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)

# NtfsDir runs on Windows and Linux (examples/CMakeLists.txt). NtfsDir2, the
# same listing over the other cache strategy, is Windows-only.
add_dependencies(NtfsBrowserTests NtfsDir)
target_compile_definitions(
  NtfsBrowserTests
  PRIVATE NTFS_DIR_EXE="$<TARGET_FILE:NtfsDir>"
)

if(WIN32)
  add_dependencies(NtfsBrowserTests NtfsDir2)
  target_compile_definitions(
    NtfsBrowserTests
    PRIVATE NTFS_DIR2_EXE="$<TARGET_FILE:NtfsDir2>"
  )

  # Win32DiskReader only exists on Windows.
  target_sources(
    NtfsBrowserTests
    PRIVATE "win32-disk-reader-offset-tests.cpp" "win32-disk-reader-tests.cpp"
  )

  # NtfsUndel's search and recovery logic (examples/ntfsundel/ntfsundel-core.h)
  # is Windows-only, like its dialog. The tests link it directly, so the
  # dialog's MFC never enters the test binary.
  target_sources(
    NtfsBrowserTests
    PRIVATE
      "ntfsundel-advice-tests.cpp"
      "ntfsundel-file-tests.cpp"
      "ntfsundel-recover-tests.cpp"
      "ntfsundel-search-tests.cpp"
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
