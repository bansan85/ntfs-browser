target_sources(
  NtfsBrowserTests
  PRIVATE
    "directory-read-ahead-class-tests.cpp"
    "directory-read-ahead-gain-tests.cpp"
    "directory-read-ahead-lane-digest-tests.cpp"
    "directory-read-ahead-lane-digests.inc"
    "directory-read-ahead-lane-tests.cpp"
    "directory-read-ahead-support.cpp"
    "directory-read-ahead-support.h"
    "directory-read-ahead-tests.cpp"
    "disk-read-failure-tests.cpp"
    "disk-reader-prefetch-tests.cpp"
    "disk-reader-size-tests.cpp"
    "disk-reader-tests.cpp"
    "extended-path-tests.cpp"
    "file-reader-run-tests.cpp"
    "file-reader-tests.cpp"
    "file-record-extension-source-tests.cpp"
    "held-probe-groups-tests.cpp"
    "long-host-path-tests.cpp"
    "ntfsdir-name-tests.cpp"
    "include/ntfs-browser/test/io/ntfsdir-sample-support.h"
    "ntfsdir-sample-tests.cpp"
    "probe-run-plan-tests.cpp"
    "probe-store-tests.cpp"
    "probe-walk-support.h"
    "read-ahead-disk-reader-tests.cpp"
    "record-memo-tests.cpp"
    "record-offset-overflow-tests.cpp"
    "record-window-tests.cpp"
    "traverse-standing-tests.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# The read ahead tests list a fake volume through NtfsDir's own listing code.
target_link_libraries(NtfsBrowserTests PRIVATE NtfsDirCore)

# NtfsAttr and NtfsDump run on Windows and Linux too. The tests link their
# logic (examples/ntfsattr/ntfsattr-core.h, examples/ntfsdump/ntfsdump-core.h)
# and run the executables.
target_sources(
  NtfsBrowserTests
  PRIVATE "ntfsattr-core-tests.cpp" "ntfsdump-core-tests.cpp"
)
target_link_libraries(NtfsBrowserTests PRIVATE NtfsAttrCore NtfsDumpCore)
add_dependencies(NtfsBrowserTests NtfsAttr NtfsDump)
target_compile_definitions(
  NtfsBrowserTests
  PRIVATE
    NTFS_ATTR_EXE="$<TARGET_FILE:NtfsAttr>"
    NTFS_DUMP_EXE="$<TARGET_FILE:NtfsDump>"
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
    PRIVATE
      "host-media-kind-tests.cpp"
      "win32-disk-reader-offset-tests.cpp"
      "win32-disk-reader-tests.cpp"
      "win32-standing-lane-tests.cpp"
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
endif()
