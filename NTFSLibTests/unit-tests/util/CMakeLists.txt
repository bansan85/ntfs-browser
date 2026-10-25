target_sources(NtfsBrowserTests PRIVATE "small-pool-tests.cpp")

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
