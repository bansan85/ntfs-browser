target_sources(
  NtfsBrowserTests
  PRIVATE
    "boot-sector-geometry-characterization-tests.cpp"
    "coverage-volume-tests.cpp"
    "drive-letter-tests.cpp"
    "filename-compare-tests.cpp"
    "fullcache-repeated-walk-tests.cpp"
    "fullcache-walk-support.h"
    "init-exit-tests.cpp"
    "mft-list-entry-characterization-tests.cpp"
    "mft-parse-failure-volume-ok-tests.cpp"
    "no-cache-lifetime-tests.cpp"
    "volume-name-log-tests.cpp"
    "volume-options-getters-tests.cpp"
    "volume-tree-support.h"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
