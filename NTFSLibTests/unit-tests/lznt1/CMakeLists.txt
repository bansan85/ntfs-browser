target_sources(NtfsBrowserTests PRIVATE "compression-test-constants.h")

# These call Lznt1::Decompress directly or drive compressed reads end to end:
# nothing left to link against without decompression.
if(NTFS_BROWSER_ENABLE_DECOMPRESSION)
  target_sources(
    NtfsBrowserTests
    PRIVATE "coverage-lznt1-tests.cpp" "lznt1-decompress-tests.cpp"
  )
endif()

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
