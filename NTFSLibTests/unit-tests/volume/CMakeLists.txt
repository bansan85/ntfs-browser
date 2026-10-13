target_sources(
  NtfsBrowserTests
  PRIVATE
    "coverage-volume-tests.cpp"
    "filename-compare-tests.cpp"
    "init-exit-tests.cpp"
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
