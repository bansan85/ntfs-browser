add_executable(NtfsBrowserTests
    "fake-ntfs-image.h"
    "fake-ntfs-image.cpp"
    "file-reader-tests.cpp"
    "no-cache-lifetime-tests.cpp")

target_link_libraries(NtfsBrowserTests PRIVATE ${PROJECT_NAME}
                                                Catch2::Catch2WithMain)

target_include_directories(NtfsBrowserTests
                            PRIVATE "${CMAKE_SOURCE_DIR}/src")

include(Catch)
catch_discover_tests(NtfsBrowserTests)
