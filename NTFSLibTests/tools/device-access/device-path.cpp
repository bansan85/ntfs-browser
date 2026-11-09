#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/device-path.h>
#include <ntfs-browser/tool/device-access/text-convert.h>

#include "io/extended-path.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess {

namespace {

// The separator the directory part uses, as the NtfsDir directory walk
// splits on it.
constexpr wchar_t directory_separator = L'\\';

// Reports whether character separates path parts on this platform.
bool IsSeparator(wchar_t character) {
#ifdef _WIN32
  return character == L'/' || character == directory_separator;
#else
  return character == L'/';
#endif
}

// What path is, among the things a volume can be read from: Device for a block
// device, File for a regular file, Missing for anything else. On Windows the
// path is extended first: the standard library hands it to Win32 as it is, and
// a path of more than 260 units would not be found.
TargetKind VolumeSourceKind(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::status(
      NTFS_BROWSER_NAMESPACE::Io::ToExtendedLengthFsPath(path), error);
  if (error) {
    return TargetKind::Missing;
  }
  if (std::filesystem::is_block_file(status)) {
    return TargetKind::Device;
  }
  return std::filesystem::is_regular_file(status) ? TargetKind::File
                                                  : TargetKind::Missing;
}

// Reports whether path names something a volume can be read from.
bool IsVolumeSource(const std::filesystem::path& path) {
  return VolumeSourceKind(path) != TargetKind::Missing;
}

// Position just after the last non-separator character of text, so trailing
// separators fall outside it.
size_t EndWithoutSeparators(std::wstring_view text) {
  size_t end = text.size();
  while (end > 0 && IsSeparator(text.at(end - 1))) {
    end--;
  }
  return end;
}

// The directory part of a target: rest with every separator turned into
// '\' and the leading and trailing ones removed.
std::wstring NormalizeDirectory(std::wstring_view rest) {
  std::wstring directory(rest);
  std::ranges::replace(directory, L'/', directory_separator);

  const size_t first = directory.find_first_not_of(directory_separator);
  if (first == std::wstring::npos) {
    return {};
  }
  const size_t last = directory.find_last_not_of(directory_separator);
  return directory.substr(first, last - first + 1);
}

// Whether a character is one of the 26 ASCII letters a drive letter is.
bool IsAsciiLetter(wchar_t character) {
  return (character >= L'a' && character <= L'z') ||
         (character >= L'A' && character <= L'Z');
}

// Whether a target that starts with a letter names a drive and nothing more:
// "c", "c:", "c:\" and "c:/" do; "c:\dir" and "c:img" do not.
bool IsBareDrive(std::wstring_view target) {
  switch (target.size()) {
    case 1:
      return true;
    case 2:
      return target[1] == L':';
    case 3:
      return target[1] == L':' &&
             (target[2] == directory_separator || target[2] == L'/');
    default:
      return false;
  }
}

}  // namespace

std::optional<wchar_t> DriveLetterOf(std::wstring_view target) {
  if (target.empty() || !IsAsciiLetter(target.front()) ||
      !IsBareDrive(target)) {
    return std::nullopt;
  }
  return target.front();
}

TargetKind ClassifyTarget(std::wstring_view target) {
#ifdef _WIN32
  if (target.starts_with(LR"(\\.\)")) {
    return TargetKind::Device;
  }
  if (DriveLetterOf(target)) {
    return TargetKind::Drive;
  }
#endif
  if (target.empty()) {
    return TargetKind::Missing;
  }
  return VolumeSourceKind(PathFromWide(target));
}

std::optional<DeviceTarget> SplitDeviceTarget(std::wstring_view target) {
  size_t end = EndWithoutSeparators(target);
  while (end > 0) {
    const std::wstring_view prefix = target.substr(0, end);
    const std::filesystem::path candidate = PathFromWide(prefix);
    if (IsVolumeSource(candidate)) {
      return DeviceTarget{.device = candidate,
                          .directory = NormalizeDirectory(target.substr(end))};
    }

    // Retry without the last path part.
    const auto separator =
        std::ranges::find_if(prefix.rbegin(), prefix.rend(), IsSeparator);
    if (separator == prefix.rend()) {
      break;
    }
    end = EndWithoutSeparators(
        prefix.substr(0, static_cast<size_t>(prefix.rend() - separator) - 1));
  }
  return std::nullopt;
}

std::optional<std::filesystem::path> TargetSource(std::wstring_view target) {
  if (const std::optional<DeviceTarget> split = SplitDeviceTarget(target)) {
    return split->device;
  }
#ifdef _WIN32
  if (target.size() >= 2 && target[1] == L':' &&
      ((target[0] >= L'a' && target[0] <= L'z') ||
       (target[0] >= L'A' && target[0] <= L'Z')) &&
      (target.size() == 2 || target[2] == L'\\' || target[2] == L'/')) {
    return std::filesystem::path(std::wstring{target[0], L':', L'\\'});
  }
#endif
  return std::nullopt;
}

std::wstring ExtendedPath(std::wstring_view path) {
  return NTFS_BROWSER_NAMESPACE::Io::ToExtendedLengthPath(path);
}

std::filesystem::path ExtendedFsPath(const std::filesystem::path& path) {
  return NTFS_BROWSER_NAMESPACE::Io::ToExtendedLengthFsPath(path);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess
