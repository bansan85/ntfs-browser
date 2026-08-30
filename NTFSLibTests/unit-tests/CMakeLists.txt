add_executable(NtfsBrowserTests
    "fake-ntfs-image.h"
    "fake-ntfs-image.cpp"
    "memory-disk-reader.h"
    "memory-disk-reader.cpp"
    "sequential-disk-reader.h"
    "sequential-disk-reader.cpp"
    "disk-reader-tests.cpp"
    "file-reader-tests.cpp"
    "no-cache-lifetime-tests.cpp")

target_link_libraries(NtfsBrowserTests PRIVATE ${PROJECT_NAME}
                                                Catch2::Catch2WithMain)

# Needed to build a synthetic NTFS image directly from the library's own
# on-disk struct layouts (src/data, src/attr) instead of hand-picked offsets.
target_include_directories(NtfsBrowserTests
                            PRIVATE "${CMAKE_SOURCE_DIR}/src")

include(Catch)
catch_discover_tests(NtfsBrowserTests)
