target_sources(
  NtfsBrowserTests
  PRIVATE
    "coverage-log-tests.cpp"
    "emit-failure-tests.cpp"
    "logging-child-process-tests.cpp"
    "logging-test-support.h"
    "logging-tests.cpp"
    "volume-options-log-tests.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
