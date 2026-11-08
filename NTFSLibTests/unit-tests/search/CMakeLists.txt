target_sources(
  NtfsBrowserTests
  PRIVATE
    "entry-buffer-tests.cpp"
    "name-matcher-tests.cpp"
    "scan-directory-tests.cpp"
    "scan-fake-engine.h"
    "scan-filter-tests.cpp"
    "scan-mft-list-tests.cpp"
    "scan-pattern-tests.cpp"
    "scan-poll-tests.cpp"
    "scan-support.cpp"
    "scan-support.h"
    "scan-thread-tests.cpp"
    "search-types-tests.cpp"
    "target-tests.cpp"
)

if(NTFS_BROWSER_ENABLE_REGEX)
  target_sources(
    NtfsBrowserTests
    PRIVATE "regex-matcher-tests.cpp" "scan-regex-tests.cpp"
  )
endif()
