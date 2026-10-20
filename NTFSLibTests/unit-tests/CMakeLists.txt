# The sources live in one folder per library namespace. Each folder's
# CMakeLists.txt adds its own files (and any feature gate they need) to this
# one executable.
add_executable(NtfsBrowserTests)

add_subdirectory("alloc")
add_subdirectory("attr")
add_subdirectory("cache")
add_subdirectory("corpus")
add_subdirectory("data")
add_subdirectory("device-access")
add_subdirectory("efs")
add_subdirectory("fuzz-regression")
add_subdirectory("io")
add_subdirectory("log")
add_subdirectory("lznt1")
add_subdirectory("mft")
add_subdirectory("ntfscompare")
add_subdirectory("record")
add_subdirectory("support")
add_subdirectory("upcase")
add_subdirectory("usn")
add_subdirectory("utf")
add_subdirectory("volume")

# spdlog: the tests of log/spdlog-adapter.h build spdlog sinks and loggers of
# their own. The library keeps spdlog private, so the tests link it too.
# frozen: fuzzer-regression-tests.cpp builds its own expected_error_messages map
# with it, independent of the library's own (private) use of frozen.
# Threads::Threads is imported in src/, so it is not visible from this folder.
find_package(Threads REQUIRED)

target_link_libraries(
  NtfsBrowserTests
  PRIVATE
    ${PROJECT_NAME}
    NtfsFakeImage
    NtfsFuzzCore
    NtfsTestDiskReaders
    Catch2::Catch2WithMain
    spdlog::spdlog
    frozen::frozen
    Threads::Threads
)

# These tests reach several internal (non-public) NtfsBrowser symbols directly,
# by including their src/ headers (see the target_include_directories below): on
# a shared build, that only links if the library itself was built exporting
# them. NTFS_BROWSER_EXPORT_INTERNALS_FOR_TESTS (src/internal-export.h) is what
# makes that conditional, so it must be defined identically on both the library
# and this target - a normal build of NtfsBrowser never defines it, keeping
# those symbols out of its export table. The library's own define lives in
# tools/fake-image/CMakeLists.txt.
target_compile_definitions(
  NtfsBrowserTests
  PRIVATE NTFS_BROWSER_EXPORT_INTERNALS_FOR_TESTS
)

# Crypto++: efs/efs-test-support.cpp's reference cipher for building EFS test
# fixtures, and support/md5-test-support.cpp's MD5. Without it, both fall back to
# BCrypt (Windows only, linked below). The cryptopp::cryptopp target only
# exists when the root CMakeLists.txt configured Crypto++, that is when
# NTFS_BROWSER_ENABLE_EFS_CRYPTOPP is on.
if(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP)
  target_link_libraries(NtfsBrowserTests PRIVATE cryptopp::cryptopp)
endif()

# BCrypt: md5-test-support.cpp's fallback MD5 backend (used by
# dftt-ntfs-tests.cpp and nps-ntfs1-tests.cpp to check recovered file
# content) when NTFS_BROWSER_ENABLE_EFS_CRYPTOPP is off. Mirrors that file's
# own "#elif defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)" -
# linked whenever that branch could be taken, whether or not Crypto++ is also
# available, since an unused import library is harmless.
if(WIN32 AND NTFS_BROWSER_ENABLE_EFS_BCRYPT)
  target_link_libraries(NtfsBrowserTests PRIVATE bcrypt)
endif()

# The fuzz/ include lets the find-stream-named and find-subentry-gap-collation
# tests reuse the fuzzer's probe headers instead of duplicating them.
target_include_directories(
  NtfsBrowserTests
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../fuzz"
)

# Avoids hardcoding NtfsFuzzerAfl's build-output path in the test.
add_dependencies(NtfsBrowserTests NtfsFuzzerAfl)
target_compile_definitions(
  NtfsBrowserTests
  PRIVATE
    NTFS_FUZZER_AFL_EXE="$<TARGET_FILE:NtfsFuzzerAfl>"
    NTFS_FUZZ_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fuzz/data"
    NTFS_TEST_TRACE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fuzz/data/corpus-traces"
    NTFS_EFS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/efs/data"
)

# Forensic image corpora (corpus-test-support.h), outside this repo.
# .github/scripts/fetch-test-data.py fills NTFS_BROWSER_TEST_DATA_DIR. Each
# per-corpus variable, when set, overrides its default under that root; so does
# an environment variable of the same name.
set(
  NTFS_BROWSER_TEST_DATA_DIR
  "${PROJECT_SOURCE_DIR}/test-data"
  CACHE PATH
  "Root of the forensic image corpora"
)
set(
  NTFS_BROWSER_TEST_DFTT_DIR
  ""
  CACHE PATH
  "DFTT images (empty: <NTFS_BROWSER_TEST_DATA_DIR>/dftt)"
)
set(
  NTFS_BROWSER_TEST_NPS_NTFS1_DIR
  ""
  CACHE PATH
  "nps-2009-ntfs1 raw image (empty: <NTFS_BROWSER_TEST_DATA_DIR>/nps-2009-ntfs1)"
)
set(
  NTFS_BROWSER_TEST_NTFS_SAMPLES_DIR
  ""
  CACHE PATH
  "ntfs-samples raw images (empty: <NTFS_BROWSER_TEST_DATA_DIR>/ntfs-samples)"
)
set(
  NTFS_BROWSER_TEST_FRAGMENTED_MFT_IMAGE
  ""
  CACHE FILEPATH
  "ntfs_extremely_fragmented_mft.raw (empty: in the ntfs-samples directory)"
)
option(
  NTFS_BROWSER_REQUIRE_TEST_DATA
  "Fail, instead of skip, a corpus test whose image fetch-test-data.py provides"
  OFF
)

set(test_DFTT_DIR "${NTFS_BROWSER_TEST_DATA_DIR}/dftt")
set(test_NPS_NTFS1_DIR "${NTFS_BROWSER_TEST_DATA_DIR}/nps-2009-ntfs1")
set(test_NTFS_SAMPLES_DIR "${NTFS_BROWSER_TEST_DATA_DIR}/ntfs-samples")
# The cache variable wins, then the same-named environment variable, then the
# default. The environment lets one machine's paths reach every preset.
foreach(
  name
  IN
  ITEMS DFTT_DIR NPS_NTFS1_DIR NTFS_SAMPLES_DIR FRAGMENTED_MFT_IMAGE
)
  # Its default depends on NTFS_SAMPLES_DIR, resolved by the previous pass.
  if(name STREQUAL "FRAGMENTED_MFT_IMAGE")
    set(
      test_${name}
      "${test_NTFS_SAMPLES_DIR}/ntfs_extremely_fragmented_mft.raw"
    )
  endif()
  if(NTFS_BROWSER_TEST_${name})
    set(test_${name} "${NTFS_BROWSER_TEST_${name}}")
  elseif(NOT "$ENV{NTFS_BROWSER_TEST_${name}}" STREQUAL "")
    set(test_${name} "$ENV{NTFS_BROWSER_TEST_${name}}")
  endif()
  # A backslash would become a C escape sequence.
  file(TO_CMAKE_PATH "${test_${name}}" test_${name})
endforeach()
target_compile_definitions(
  NtfsBrowserTests
  PRIVATE
    NTFS_TEST_DFTT_DIR="${test_DFTT_DIR}"
    NTFS_TEST_NPS_NTFS1_DIR="${test_NPS_NTFS1_DIR}"
    NTFS_TEST_NTFS_SAMPLES_DIR="${test_NTFS_SAMPLES_DIR}"
    NTFS_TEST_FRAGMENTED_MFT_IMAGE="${test_FRAGMENTED_MFT_IMAGE}"
    $<$<BOOL:${NTFS_BROWSER_REQUIRE_TEST_DATA}>:NTFS_TEST_REQUIRE_DATA>
)

# Mirrors the library's own compile definitions: the EFS tests' #ifdefs live in
# this target, not the library's, so it needs the same three macros.
target_compile_definitions(
  NtfsBrowserTests
  PRIVATE
    $<$<BOOL:${NTFS_BROWSER_ENABLE_DECOMPRESSION}>:NTFS_BROWSER_ENABLE_DECOMPRESSION>
    $<$<BOOL:${NTFS_BROWSER_ENABLE_EFS_CRYPTOPP}>:NTFS_BROWSER_ENABLE_EFS_CRYPTOPP>
    $<$<BOOL:${NTFS_BROWSER_ENABLE_EFS_BCRYPT}>:NTFS_BROWSER_ENABLE_EFS_BCRYPT>
)

if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsBrowserTests
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:NtfsBrowserTests>
      $<TARGET_FILE_DIR:NtfsBrowserTests>
    COMMAND_EXPAND_LISTS
  )
endif()

include(Catch)
catch_discover_tests(NtfsBrowserTests)
