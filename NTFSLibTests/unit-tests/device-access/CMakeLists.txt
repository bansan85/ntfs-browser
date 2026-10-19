# NtfsDeviceAccess (tools/device-access/), tested on temp files and strings.
target_sources(
  NtfsBrowserTests
  PRIVATE
    "device-path-tests.cpp"
    "temp-dir.h"
    "text-convert-tests.cpp"
    "tool-support-tests.cpp"
)

target_link_libraries(NtfsBrowserTests PRIVATE NtfsDeviceAccess)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
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
      "local-time-tests.cpp"
      "posix-device-disk-reader-tests.cpp"
  )
endif()
