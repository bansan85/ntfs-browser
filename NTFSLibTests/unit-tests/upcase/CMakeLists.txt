target_sources(
  NtfsBrowserTests
  PRIVATE
    "coverage-upcase-fallback-tests.cpp"
    "coverage-upcase-tests.cpp"
    "upcase-collation-tests.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
