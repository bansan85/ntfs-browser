target_sources(
  NtfsBrowserTests
  PRIVATE "chunk-pool-tests.cpp" "small-pool-tests.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
