target_sources(
  NtfsBrowserTests
  PRIVATE
    "boot-sector-tests.cpp"
    "data-run-lcn-range-tests.cpp"
    "index-entry-tests.cpp"
    "on-disk-alignment-tests.cpp"
    "on-disk-struct-layout-tests.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
