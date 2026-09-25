set(NtfsBrowserTests_SRC
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
    "attr-vol-info-size-tests.cpp"
    "attr-std-info-legacy-size-tests.cpp"
    "on-disk-struct-layout-tests.cpp"
    "file-record-unparsed-flags-tests.cpp"
    "fragmented-mft-invalid-record-tests.cpp"
    "find-stream-named-tests.cpp"
    "index-root-entry-lifetime-tests.cpp"
    "find-subentry-gap-collation-tests.cpp"
    "index-block-recursion-depth-tests.cpp"
    "traverse-sub-entries-recovery-tests.cpp"
    "mft-tree-tests.cpp"
    "volume-options-tests.cpp"
    "fuzzer-regression-tests.cpp"
    "logging-tests.cpp"
    "utf-tests.cpp"
    "efs-disabled-tests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../fuzz/named-stream-probe.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../fuzz/gap-collation-probe.h")

# attr-compression-tests.cpp calls Lznt1::Decompress directly and drives
# compressed reads end to end: nothing left to link against without it.
if(NTFS_BROWSER_ENABLE_DECOMPRESSION)
  list(APPEND NtfsBrowserTests_SRC "attr-compression-tests.cpp")
endif()

# Same "EFS master" condition (NTFS_BROWSER_EFS_MASTER, set in the root
# CMakeLists.txt) as the library's own source-list gate: these files must build
# only when the library they link against actually compiled efs-context.cpp et
# al. in.
if(NTFS_BROWSER_EFS_MASTER)
  list(APPEND NtfsBrowserTests_SRC "efs-test-support.h" "efs-test-support.cpp"
       "efs-pfx-tests.cpp" "efs-tests.cpp")
endif()

add_executable(NtfsBrowserTests ${NtfsBrowserTests_SRC})

# spdlog: the capturing sink the tests install on the library's logger is an
# spdlog sink. The library keeps spdlog private, so the tests link it too.
# frozen: fuzzer-regression-tests.cpp builds its own kExpectedErrorMessages map
# with it, independent of the library's own (private) use of frozen.
target_link_libraries(
  NtfsBrowserTests PRIVATE ${PROJECT_NAME} Catch2::Catch2WithMain
                           spdlog::spdlog frozen::frozen)

# These tests reach several internal (non-public) NtfsBrowser symbols directly,
# by including their src/ headers (see the target_include_directories below): on
# a shared build, that only links if the library itself was built exporting
# them. NTFS_BROWSER_EXPORT_INTERNALS_FOR_TESTS (src/internal-export.h) is what
# makes that conditional, so it must be defined identically on both the library
# and this target - a normal build of NtfsBrowser never defines it, keeping
# those symbols out of its export table.
target_compile_definitions(${PROJECT_NAME}
                           PRIVATE NTFS_BROWSER_EXPORT_INTERNALS_FOR_TESTS)
target_compile_definitions(NtfsBrowserTests
                           PRIVATE NTFS_BROWSER_EXPORT_INTERNALS_FOR_TESTS)

# Crypto++: efs-test-support.cpp's own reference cipher for building EFS test
# fixtures, independent of NTFS_BROWSER_ENABLE_EFS_CRYPTOPP (the library's own
# use of Crypto++) - not narrowed to it, unlike the library's link above, since
# a BCrypt-only library configuration still needs this reference to test
# against. It follows the same "EFS master" condition the root CMakeLists.txt
# uses to decide whether Crypto++ is configured at all: that target does not
# exist when the condition is false, and efs-test-support.cpp is excluded from
# the source list above in that case too, so nothing here needs it then.
if(NTFS_BROWSER_EFS_MASTER)
  target_link_libraries(NtfsBrowserTests PRIVATE cryptopp::cryptopp)
endif()

# The fuzz/ include lets fake-ntfs-image.h reuse named-stream-probe.h's name
# instead of duplicating it.
target_include_directories(
  NtfsBrowserTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
                           "${CMAKE_CURRENT_SOURCE_DIR}/../fuzz")

# Avoids hardcoding NtfsFuzzerAfl's build-output path in the test.
add_dependencies(NtfsBrowserTests NtfsFuzzerAfl)
target_compile_definitions(
  NtfsBrowserTests
  PRIVATE NTFS_FUZZER_AFL_EXE="$<TARGET_FILE:NtfsFuzzerAfl>"
          NTFS_FUZZ_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fuzz/data"
          NTFS_EFS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
# Mirrors the library's own compile definitions: efs-tests.cpp's #ifdefs live in
# this target, not the library's, so it needs the same three macros.
target_compile_definitions(
  NtfsBrowserTests
  PRIVATE
    $<$<BOOL:${NTFS_BROWSER_ENABLE_DECOMPRESSION}>:NTFS_BROWSER_ENABLE_DECOMPRESSION>
    $<$<BOOL:${NTFS_BROWSER_ENABLE_EFS_CRYPTOPP}>:NTFS_BROWSER_ENABLE_EFS_CRYPTOPP>
    $<$<BOOL:${NTFS_BROWSER_ENABLE_EFS_BCRYPT}>:NTFS_BROWSER_ENABLE_EFS_BCRYPT>)

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
