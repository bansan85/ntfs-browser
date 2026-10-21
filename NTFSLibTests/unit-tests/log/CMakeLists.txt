target_sources(
  NtfsBrowserTests
  PRIVATE
    "coverage-log-tests.cpp"
    "custom-sink-tests.cpp"
    "emit-failure-tests.cpp"
    "log-level-edge-tests.cpp"
    "logging-child-process-tests.cpp"
    "logging-test-support.h"
    "logging-tests.cpp"
    "rejection-cost-tests.cpp"
    "spdlog-adapter-tests.cpp"
    "volume-options-log-tests.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
