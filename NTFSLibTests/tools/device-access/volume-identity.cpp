#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/text-convert.h>
#include <ntfs-browser/tool/device-access/volume-identity.h>

#ifdef _WIN32
  #include <array>
  #include <cwctype>
  #include <format>

  #include <windows.h>
#else
  #include <sys/stat.h>
#endif

namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess {

namespace {

// The path itself when it exists, else the nearest parent that does. The path
// is made absolute first, so a relative name starts at the current folder.
std::filesystem::path NearestExisting(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::path current = std::filesystem::absolute(path, error);
  if (error) {
    return path;
  }
  while (!std::filesystem::exists(current, error) &&
         current.has_parent_path() && current != current.parent_path()) {
    current = current.parent_path();
  }
  return current;
}

#ifdef _WIN32

// The prefix of a device path. The text is a raw string, so the backslashes
// are as written.
constexpr std::wstring_view device_prefix = LR"(\\.\)";

// Room for a volume root or a GUID name, which is 49 characters plus the end.
constexpr std::size_t volume_name_capacity = 64;

// Room for a path that GetVolumePathNameW() returns: the longest extended
// path.
constexpr std::size_t volume_path_capacity = 32768;

// The root of the drive that "\\.\X:" names, or empty for any other text.
std::wstring RootOfDeviceDrive(const std::wstring& text) {
  if (!text.starts_with(device_prefix)) {
    return {};
  }
  const std::wstring rest = text.substr(device_prefix.size());
  const bool is_drive = rest.size() >= 2 && std::iswalpha(rest[0]) != 0 &&
                        rest[1] == L':' && rest.size() <= 3;
  return is_drive ? std::wstring{rest[0], L':', L'\\'} : std::wstring{};
}

// The root of the volume that holds path, with a final backslash.
std::wstring VolumeRoot(const std::filesystem::path& path) {
  const std::wstring root = RootOfDeviceDrive(path.wstring());
  if (!root.empty()) {
    return root;
  }
  if (path.wstring().starts_with(device_prefix)) {
    return {};
  }
  std::wstring buffer(volume_path_capacity, L'\0');
  if (GetVolumePathNameW(path.c_str(), buffer.data(),
                         static_cast<DWORD>(buffer.size())) == FALSE) {
    return {};
  }
  buffer.resize(buffer.find(L'\0'));
  return buffer;
}

#endif  // _WIN32

}  // namespace

std::optional<std::string> HostVolumeId(const std::filesystem::path& path) {
#ifdef _WIN32
  const std::wstring root = VolumeRoot(NearestExisting(path));
  if (root.empty()) {
    return std::nullopt;
  }

  std::array<wchar_t, volume_name_capacity> guid{};
  if (GetVolumeNameForVolumeMountPointW(root.c_str(), guid.data(),
                                        static_cast<DWORD>(guid.size())) !=
      FALSE) {
    return WideToUtf8(guid.data());
  }
  // A drive made with subst has no GUID name, but it has a serial number.
  DWORD serial = 0;
  if (GetVolumeInformationW(root.c_str(), nullptr, 0, &serial, nullptr, nullptr,
                            nullptr, 0) != FALSE) {
    return std::format("serial:{:08X}", serial);
  }
  return std::nullopt;
#else
  struct stat info{};
  if (stat(NearestExisting(path).c_str(), &info) != 0) {
    return std::nullopt;
  }
  const dev_t device = S_ISBLK(info.st_mode) ? info.st_rdev : info.st_dev;
  return "dev:" + std::to_string(static_cast<unsigned long long>(device));
#endif
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess
