target_sources(
  NtfsBrowserTests
  PRIVATE
    "attr-bitmap-tests.cpp"
    "attr-body-size-tests.cpp"
    "attr-cache-release-tests.cpp"
    "attr-initialized-size-tests.cpp"
    "attr-name-bounds-tests.cpp"
    "attr-read-data-bounded-tests.cpp"
    "attr-resident-read-data-tests.cpp"
    "attr-run-list-tests.cpp"
    "attribute-list-entries-tests.cpp"
    "attribute-list-support.h"
    "attribute-list-tests.cpp"
    "coverage-attrs-tests.cpp"
    "coverage-nonresident-index-tests.cpp"
    "index-entry-size-tests.cpp"
    "nonresident-index-support.h"
)

# These call Lznt1::Decompress directly or drive compressed reads end to end:
# nothing left to link against without decompression.
if(NTFS_BROWSER_ENABLE_DECOMPRESSION)
  target_sources(NtfsBrowserTests PRIVATE "attr-compression-tests.cpp")
endif()

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
