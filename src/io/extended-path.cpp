#include <filesystem>
#include <string>
#include <string_view>

#include <ntfs-browser/io/extended-path.h>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Io {

#ifdef _WIN32

namespace {

// Starts an extended-length path: Win32 hands the rest to the file system
// as it is.
constexpr std::wstring_view extended_prefix = L"\\\\?\\";

// Starts a path in the device namespace: \\.\PhysicalDrive0, \\.\C:.
constexpr std::wstring_view device_prefix = L"\\\\.\\";

// Replaces the two backslashes that start a UNC path.
constexpr std::wstring_view extended_unc_prefix = L"\\\\?\\UNC\\";

// Starts a UNC path: \\server\share.
constexpr std::wstring_view unc_prefix = L"\\\\";

// Capacity GetFullPathNameW is first given. Most paths fit it, so one call
// is the usual cost.
constexpr size_t first_full_path_capacity = 512;

// Absolute and normalised form of path, or nothing when the system cannot
// resolve it.
std::wstring FullPath(const std::wstring& path) {
  std::wstring full(first_full_path_capacity, L'\0');
  DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()),
                                  full.data(), nullptr);
  if (length >= full.size()) {
    // The call reports the size it needs, terminator included.
    full.resize(length);
    length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()),
                              full.data(), nullptr);
  }
  if (length == 0 || length >= full.size()) {
    return {};
  }
  full.resize(length);
  return full;
}

}  // namespace

std::wstring ToExtendedLengthPath(std::wstring_view path) {
  if (path.empty() || path.starts_with(extended_prefix) ||
      path.starts_with(device_prefix)) {
    return std::wstring(path);
  }

  // A view carries no NUL terminator, and GetFullPathNameW needs one.
  const std::wstring terminated(path);
  std::wstring full = FullPath(terminated);
  if (full.empty()) {
    return terminated;
  }
  // The call can turn a reserved name (NUL, COM1) into a device path.
  if (full.starts_with(extended_prefix) || full.starts_with(device_prefix)) {
    return full;
  }
  if (full.starts_with(unc_prefix)) {
    return std::wstring(extended_unc_prefix) + full.substr(unc_prefix.size());
  }
  return std::wstring(extended_prefix) + full;
}

#else

std::wstring ToExtendedLengthPath(std::wstring_view path) {
  return std::wstring(path);
}

#endif  // _WIN32

std::filesystem::path
    ToExtendedLengthFsPath(const std::filesystem::path& path) {
#ifdef _WIN32
  return ToExtendedLengthPath(std::wstring_view(path.native()));
#else
  return path;
#endif
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
