#ifndef _WIN32

  #include <cstdlib>
  #include <filesystem>

  #include "host-media.h"

namespace {

// Runs before main(): the sysfs tree is a folder that holds nothing, so that
// the first test already asks no real disk about its image file. A test that
// wants a tree makes its own and points the root at it.
[[maybe_unused]] const bool g_installed = [] {
  NtfsDevice::SetSysfsRootForTests(std::filesystem::path("/nonexistent-sysfs"));
  // The tools that tests run as child processes read the variable instead.
  setenv("NTFS_BROWSER_NO_HOST_MEDIA_QUERY", "1", 1);
  return true;
}();

}  // namespace

#endif  // !_WIN32
