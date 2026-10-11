target_sources(NtfsBrowserTests PRIVATE "fuzzer-regression-tests.cpp")

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
