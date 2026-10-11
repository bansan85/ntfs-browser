target_sources(
  NtfsBrowserTests
  PRIVATE
    "disk-read-failure-tests.cpp"
    "disk-reader-tests.cpp"
    "file-reader-tests.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
