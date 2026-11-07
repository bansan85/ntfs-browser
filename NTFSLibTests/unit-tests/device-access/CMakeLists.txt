# NtfsDeviceAccess (tools/device-access/), tested on temp files and strings.
target_sources(
  NtfsBrowserTests
  PRIVATE
    "apply-tuning-tests.cpp"
    "bench-options-tests.cpp"
    "bench-tools-tests.cpp"
    "device-path-tests.cpp"
    "json-tests.cpp"
    "ntfs-bench-tool-tests.cpp"
    "ordered-chunks-tests.cpp"
    "output-tests.cpp"
    "profile-tools-tests.cpp"
    "sparse-capture-tests.cpp"
    "include/ntfs-browser/test/device-access/temp-dir.h"
    "text-convert-tests.cpp"
    "tool-support-tests.cpp"
)

target_link_libraries(NtfsBrowserTests PRIVATE NtfsDeviceAccess)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# The tests of NtfsBench run the executable (tools/benchmark/).
add_dependencies(NtfsBrowserTests NtfsBench)
target_compile_definitions(
  NtfsBrowserTests
  PRIVATE NTFS_BENCH_EXE="$<TARGET_FILE:NtfsBench>"
)

# The POSIX reader, the argv adapter, the Linux driver names and the POSIX
# local-time branch only exist off Windows. The Windows branch of the local time is covered by the
# sample tests, which compare against the Win32 conversion.
if(NOT WIN32)
  target_sources(
    NtfsBrowserTests
    PRIVATE
      "argv-wide-tests.cpp"
      "driver-names-tests.cpp"
      "host-media-tests.cpp"
      "local-time-tests.cpp"
      "posix-device-disk-reader-tests.cpp"
  )
endif()
