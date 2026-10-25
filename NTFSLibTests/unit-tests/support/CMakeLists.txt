target_sources(
  NtfsBrowserTests
  PRIVATE
    "child-process.cpp"
    "child-process.h"
    "child-process-tests.cpp"
    "md5-test-support.cpp"
    "md5-test-support.h"
    "optional-access.h"
    "sample-run.cpp"
    "sample-run.h"
    "scan-comparison.cpp"
    "scan-comparison.h"
    "scratch-record.h"
    "scripted-disk-reader.h"
    "test-log-sink.cpp"
    "test-log-sink.h"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
