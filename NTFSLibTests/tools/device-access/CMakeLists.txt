# Platform helpers shared by NtfsDir, NtfsAttr, NtfsDump, NtfsUndel,
# NtfsScanBench and NtfsCompare: a POSIX device reader, UTF-8 <-> wide
# conversion, local time, device-path splitting, volume opening from a target,
# the host volume of a path, the argv adapter and the buffered listing output. Portable C++: the POSIX halves sit behind
# #ifndef _WIN32, the Windows halves behind #ifdef _WIN32.
add_library(
  NtfsDeviceAccess
  STATIC
  "include/ntfs-browser/tool/device-access/argv-wide.h"
  "argv-wide.cpp"
  "include/ntfs-browser/tool/device-access/bench-options.h"
  "bench-options.cpp"
  "include/ntfs-browser/tool/device-access/bench-session.h"
  "bench-session.cpp"
  "include/ntfs-browser/tool/device-access/device-path.h"
  "device-path.cpp"
  "include/ntfs-browser/tool/device-access/disk-identity.h"
  "disk-identity.cpp"
  "include/ntfs-browser/tool/device-access/driver-names.h"
  "driver-names.cpp"
  "include/ntfs-browser/tool/device-access/host-media.h"
  "host-media.cpp"
  "include/ntfs-browser/tool/device-access/json.h"
  "json.cpp"
  "include/ntfs-browser/tool/device-access/line-buffer.h"
  "line-buffer.cpp"
  "include/ntfs-browser/tool/device-access/local-time.h"
  "local-time.cpp"
  "include/ntfs-browser/tool/device-access/ordered-chunks.h"
  "ordered-chunks.cpp"
  "include/ntfs-browser/tool/device-access/output.h"
  "output.cpp"
  "include/ntfs-browser/tool/device-access/posix-device-disk-reader.h"
  "posix-device-disk-reader.cpp"
  "include/ntfs-browser/tool/device-access/sparse-capture.h"
  "sparse-capture.cpp"
  "include/ntfs-browser/tool/device-access/text-convert.h"
  "text-convert.cpp"
  "include/ntfs-browser/tool/device-access/tool-support.h"
  "tool-support.cpp"
  "include/ntfs-browser/tool/device-access/volume-identity.h"
  "volume-identity.cpp"
  "include/ntfs-browser/tool/device-access/volume-target.h"
  "volume-target.cpp"
)
target_include_directories(
  NtfsDeviceAccess
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(NtfsDeviceAccess PUBLIC NtfsBrowserInternals)
# nlohmann/json stays in json.cpp: json.h includes none of it.
target_link_libraries(NtfsDeviceAccess PRIVATE nlohmann_json::nlohmann_json)
