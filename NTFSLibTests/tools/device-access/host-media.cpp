#ifndef _WIN32

  #include <filesystem>
  #include <format>
  #include <fstream>
  #include <optional>
  #include <string>
  #include <system_error>
  #include <utility>

  #include <ntfs-browser/tool/device-access/host-media.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess {

namespace {

// The first line of a sysfs file, or empty.
std::string ReadLine(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  return line;
}

// The sysfs tree in use. Empty means the real one.
std::filesystem::path& OverriddenRoot() {
  static std::filesystem::path root;
  return root;
}

}  // namespace

std::optional<bool> RotationalOfBlockDevice(unsigned major, unsigned minor,
                                            const std::filesystem::path& root) {
  namespace fs = std::filesystem;
  std::error_code error;
  // dev/block/<major>:<minor> links to .../block/<disk>[/<partition>].
  fs::path node = fs::canonical(
      root / "dev" / "block" / std::format("{}:{}", major, minor), error);
  if (error) {
    return std::nullopt;
  }
  if (fs::exists(node / "partition", error)) {
    node = node.parent_path();
  }
  const std::string rotational = ReadLine(node / "queue" / "rotational");
  if (rotational.empty()) {
    return std::nullopt;
  }
  return rotational != "0";
}

const std::filesystem::path& SysfsRoot() {
  static const std::filesystem::path real_root("/sys");
  const std::filesystem::path& overridden = OverriddenRoot();
  return overridden.empty() ? real_root : overridden;
}

void SetSysfsRootForTests(std::filesystem::path root) {
  OverriddenRoot() = std::move(root);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess

#endif  // !_WIN32
