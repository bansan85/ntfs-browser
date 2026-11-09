target_sources(
  NtfsBrowserTests
  PRIVATE
    "entry-buffer-tests.cpp"
    "name-matcher-tests.cpp"
    "reader-attribute-tests.cpp"
    "reader-content-tests.cpp"
    "reader-handle-tests.cpp"
    "reader-special-tests.cpp"
    "reader-support.cpp"
    "reader-support.h"
    "scan-directory-tests.cpp"
    "scan-fake-engine.h"
    "scan-filter-tests.cpp"
    "scan-mft-list-tests.cpp"
    "scan-pattern-tests.cpp"
    "scan-poll-tests.cpp"
    "scan-probe-tests.cpp"
    "scan-run-tests.cpp"
    "scan-support.cpp"
    "scan-support.h"
    "scan-thread-support.h"
    "scan-thread-tests.cpp"
    "scan-verdict-tests.cpp"
    "search-types-tests.cpp"
    "target-tests.cpp"
)

# The measure of a scan (examples/ntfsscanbench), run here on synthetic images.
target_link_libraries(NtfsBrowserTests PRIVATE NtfsScanBenchCore)

if(NTFS_BROWSER_ENABLE_REGEX)
  target_sources(
    NtfsBrowserTests
    PRIVATE "regex-matcher-tests.cpp" "scan-regex-tests.cpp"
  )
endif()
