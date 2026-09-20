add_executable(
  NtfsBrowserTests
  "fake-ntfs-image.h"
  "fake-ntfs-image.cpp"
  "memory-disk-reader.h"
  "memory-disk-reader.cpp"
  "sequential-disk-reader.h"
  "sequential-disk-reader.cpp"
  "test-log-sink.h"
  "test-log-sink.cpp"
  "disk-reader-tests.cpp"
  "file-reader-tests.cpp"
  "file-reader-full-cache-boundary-tests.cpp"
  "no-cache-lifetime-tests.cpp"
  "attribute-list-tests.cpp"
  "filename-compare-tests.cpp"
  "parse-attrs-total-size-tests.cpp"
  "file-record-header-fixup-tests.cpp"
  "file-record-header-size-tests.cpp"
  "index-block-fixup-tests.cpp"
  "boot-sector-index-block-size-tests.cpp"
  "boot-sector-oversized-record-size-tests.cpp"
  "mft-addr-narrowing-tests.cpp"
  "mft-parse-failure-volume-ok-tests.cpp"
  "attr-name-bounds-tests.cpp"
  "attr-resident-read-data-tests.cpp"
  "attr-compression-tests.cpp"
  "attr-vol-info-size-tests.cpp"
  "attr-std-info-legacy-size-tests.cpp"
  "on-disk-struct-layout-tests.cpp"
  "file-record-unparsed-flags-tests.cpp"
  "fragmented-mft-invalid-record-tests.cpp"
  "find-stream-named-tests.cpp"
  "index-root-entry-lifetime-tests.cpp"
  "find-subentry-gap-collation-tests.cpp"
  "index-block-recursion-depth-tests.cpp"
  "fuzzer-regression-tests.cpp"
  "logging-tests.cpp"
  "utf-tests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../fuzz/named-stream-probe.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/../fuzz/gap-collation-probe.h")

# spdlog: the capturing sink the tests install on the library's logger is an
# spdlog sink. The library keeps spdlog private, so the tests link it too.
target_link_libraries(
  NtfsBrowserTests PRIVATE ${PROJECT_NAME} Catch2::Catch2WithMain
                           spdlog::spdlog)

# The fuzz/ include lets fake-ntfs-image.h reuse named-stream-probe.h's name
# instead of duplicating it.
target_include_directories(
  NtfsBrowserTests PRIVATE "${CMAKE_SOURCE_DIR}/src"
                           "${CMAKE_CURRENT_SOURCE_DIR}/../fuzz")

# Avoids hardcoding NtfsFuzzerAfl's build-output path in the test.
add_dependencies(NtfsBrowserTests NtfsFuzzerAfl)
target_compile_definitions(
  NtfsBrowserTests
  PRIVATE NTFS_FUZZER_AFL_EXE="$<TARGET_FILE:NtfsFuzzerAfl>"
          NTFS_FUZZ_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fuzz/data")

if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsBrowserTests
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:NtfsBrowserTests>
      $<TARGET_FILE_DIR:NtfsBrowserTests>
    COMMAND_EXPAND_LISTS)
endif()

include(Catch)
catch_discover_tests(NtfsBrowserTests)
