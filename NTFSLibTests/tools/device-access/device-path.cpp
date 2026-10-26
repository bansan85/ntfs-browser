#include "device-path.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>

#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/namespace.h>

#include "text-convert.h"

namespace NtfsDevice {

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

// Reports whether path names something a volume can be read from. On Windows
// the path is extended first: the standard library hands it to Win32 as it
// is, and a path of more than 260 units would not be found.
bool IsVolumeSource(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::status(
      NTFS_BROWSER_NAMESPACE::Io::ToExtendedLengthFsPath(path), error);
  if (error) {
    return false;
  }
  return std::filesystem::is_regular_file(status) ||
         std::filesystem::is_block_file(status);
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

}  // namespace

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

}  // namespace NtfsDevice
