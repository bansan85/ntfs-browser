target_sources(
  NtfsBrowserTests
  PRIVATE
    "block-cache-ceiling-tests.cpp"
    "block-cache-test-support.h"
    "block-cache-tests.cpp"
    "view-tests.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
