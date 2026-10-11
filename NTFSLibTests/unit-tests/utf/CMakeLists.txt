target_sources(NtfsBrowserTests PRIVATE "utf-tests.cpp")

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
