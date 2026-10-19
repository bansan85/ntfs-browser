# Platform helpers shared by NtfsDir, NtfsMftList and NtfsCompare: a POSIX
# device reader, UTF-8 <-> wide conversion, local time, device-path splitting
# and the argv adapter. Portable C++: the POSIX halves sit behind
# #ifndef _WIN32, the Windows halves behind #ifdef _WIN32.
add_library(
  NtfsDeviceAccess
  STATIC
  "argv-wide.h"
  "argv-wide.cpp"
  "device-path.h"
  "device-path.cpp"
  "driver-names.h"
  "driver-names.cpp"
  "local-time.h"
  "local-time.cpp"
  "posix-device-disk-reader.h"
  "posix-device-disk-reader.cpp"
  "text-convert.h"
  "text-convert.cpp"
  "tool-support.h"
  "tool-support.cpp"
)
target_include_directories(
  NtfsDeviceAccess
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(NtfsDeviceAccess PUBLIC ${PROJECT_NAME})
