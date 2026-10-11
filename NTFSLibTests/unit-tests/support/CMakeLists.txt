target_sources(
  NtfsBrowserTests
  PRIVATE
    "child-process.cpp"
    "child-process.h"
    "md5-test-support.cpp"
    "md5-test-support.h"
    "optional-access.h"
    "test-log-sink.cpp"
    "test-log-sink.h"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
