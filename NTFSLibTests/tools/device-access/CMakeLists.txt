# Platform helpers shared by NtfsDir, NtfsMftList and NtfsCompare: a POSIX
# device reader, UTF-8 <-> wide conversion, local time, device-path splitting,
# the argv adapter and the buffered listing output. Portable C++: the POSIX
# halves sit behind #ifndef _WIN32, the Windows halves behind #ifdef _WIN32.
add_library(
  NtfsDeviceAccess
  STATIC
  "argv-wide.h"
  "argv-wide.cpp"
  "bench-options.h"
  "bench-options.cpp"
  "bench-session.h"
  "bench-session.cpp"
  "device-path.h"
  "device-path.cpp"
  "disk-identity.h"
  "disk-identity.cpp"
  "driver-names.h"
  "driver-names.cpp"
  "json.h"
  "json.cpp"
  "line-buffer.h"
  "line-buffer.cpp"
  "local-time.h"
  "local-time.cpp"
  "ordered-chunks.h"
  "ordered-chunks.cpp"
  "output.h"
  "output.cpp"
  "posix-device-disk-reader.h"
  "posix-device-disk-reader.cpp"
  "sparse-capture.h"
  "sparse-capture.cpp"
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
