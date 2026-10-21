target_sources(
  NtfsBrowserTests
  PRIVATE
    "corpus-test-support.cpp"
    "corpus-test-support.h"
    "dftt-ntfs-tests.cpp"
    "external-logfile-fixtures-tests.cpp"
    "external-mft-support.cpp"
    "external-mft-support.h"
    "external-mft-tests.cpp"
    "external-slack-tests.cpp"
    "nps-ntfs1-test-support.cpp"
    "nps-ntfs1-test-support.h"
    "nps-ntfs1-tests.cpp"
    "ntfs-samples-2m-tests.cpp"
    "ntfs-samples-fragmented-mft-tests.cpp"
    "ntfs-samples-ptrn-tests.cpp"
    "ntfs-samples-support.h"
    "ntfs-samples-timestamps-tests.cpp"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
