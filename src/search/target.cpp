#include "search/target.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/extended-path.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "log/ntfs-common.h"
#include "search/scan-probe.h"
#include "utf/utf.h"

#ifndef _WIN32
  #include <cerrno>

  #include "io/posix-disk-reader.h"
#endif

namespace NTFS_BROWSER_NAMESPACE::Search {

namespace {

// Separates the folders of the path inside the volume, and the parts of a
// Windows path.
constexpr wchar_t backslash = L'\\';

// Whether character separates the parts of a path of the host. A POSIX file
// name MAY hold a backslash, so only the slash does there.
bool IsHostSeparator(wchar_t character) {
#ifdef _WIN32
  return character == L'/' || character == backslash;
#else
  return character == L'/';
#endif
}

// Whether character separates the folders inside the volume. Both do on every
// host: a Win32 name holds neither.
bool IsInsideSeparator(wchar_t character) {
  return character == L'/' || character == backslash;
}

// The filesystem path of text. A POSIX host takes UTF-8, whatever its locale.
std::filesystem::path PathOf(std::wstring_view text) {
#ifdef _WIN32
  return std::filesystem::path(std::wstring(text));
#else
  const std::string utf8 = Utf::WideToUtf8(text);
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#endif
}

// Whether path names an existing regular file or, on a POSIX host, a block
// device: something a volume can be read from. The path is extended first, as
// the standard library hands it to Win32 as it is.
bool IsVolumeSource(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::status(Io::ToExtendedLengthFsPath(path), error);
  if (error) {
    return false;
  }
  return std::filesystem::is_regular_file(status) ||
         std::filesystem::is_block_file(status);
}

// The folder names of rest, cut at both separators, with no empty part.
std::vector<std::wstring> SplitInside(std::wstring_view rest) {
  std::vector<std::wstring> parts;
  std::size_t start = 0;
  while (start <= rest.size()) {
    std::size_t end = start;
    while (end < rest.size() && !IsInsideSeparator(rest[end])) {
      ++end;
    }
    if (end > start) {
      parts.emplace_back(rest.substr(start, end - start));
    }
    start = end + 1;
  }
  return parts;
}

// The length of target up to position end, with no trailing separator.
std::size_t TrimEnd(std::wstring_view target, std::size_t end) {
  while (end > 0 && IsHostSeparator(target[end - 1])) {
    --end;
  }
  return end;
}

#ifdef _WIN32

// Whether character is one of the 26 ASCII letters of a drive.
bool IsAsciiLetter(wchar_t character) {
  return (character >= L'a' && character <= L'z') ||
         (character >= L'A' && character <= L'Z');
}

#endif

}  // namespace

std::optional<TargetParts> SplitTarget(std::wstring_view target) {
  std::size_t end = TrimEnd(target, target.size());
  while (end > 0) {
    const std::wstring_view prefix = target.substr(0, end);
    if (IsVolumeSource(PathOf(prefix))) {
      return TargetParts{.kind = TargetParts::Kind::Image,
                         .source = std::wstring(prefix),
                         .inside = SplitInside(target.substr(end))};
    }
    // Retry without the last part.
    const auto separator =
        std::ranges::find_if(prefix.rbegin(), prefix.rend(), IsHostSeparator);
    if (separator == prefix.rend()) {
      break;
    }
    end = TrimEnd(target,
                  static_cast<std::size_t>(prefix.rend() - separator) - 1);
  }

#ifdef _WIN32
  if (target.size() >= 2 && IsAsciiLetter(target[0]) && target[1] == L':' &&
      (target.size() == 2 || IsInsideSeparator(target[2]))) {
    return TargetParts{.kind = TargetParts::Kind::Drive,
                       .source = std::wstring(1, target[0]),
                       .inside = SplitInside(target.substr(2))};
  }
#endif
  return std::nullopt;
}

namespace {

// Opens the file, device or drive that parts names. No volume when the open
// fails: the log says why.
template <Cache::Strategy S>
OpenedVolume<S> OpenFromTarget(const TargetParts& parts,
                               const VolumeOptions& options) {
  OpenedVolume<S> opened;
#ifdef _WIN32
  // Win32DiskReader opens a plain file the same way as a device.
  opened.volume =
      parts.kind == TargetParts::Kind::Drive
          ? std::make_unique<NtfsVolume<S>>(parts.source.front(), options)
          : std::make_unique<NtfsVolume<S>>(std::wstring_view(parts.source),
                                            options);
#else
  auto reader = std::make_unique<Io::PosixDiskReader>();
  if (!reader->Open(parts.source)) {
    const int error = reader->Error();
    if (error == EACCES || error == EPERM) {
      Log::Error("Cannot read device {}: read access is needed",
                 Utf::WideToUtf8(parts.source));
    } else {
      Log::Error("Cannot open {}", Utf::WideToUtf8(parts.source));
    }
    return opened;
  }
  opened.volume = std::make_unique<NtfsVolume<S>>(std::move(reader), options);
#endif
  return opened;
}

}  // namespace

template <Cache::Strategy S>
OpenedVolume<S> OpenVolume(const TargetParts& parts,
                           const VolumeOptions& options) {
  OpenedVolume<S> opened;
  if (const ReaderFactory& factory = ReaderFactoryOverride(); factory) {
    std::unique_ptr<IDiskReader> reader = factory(parts);
    if (!reader) {
      Log::Error("Cannot open {}", Utf::WideToUtf8(parts.source));
      return opened;
    }
    opened.volume = std::make_unique<NtfsVolume<S>>(std::move(reader), options);
  } else {
    opened = OpenFromTarget<S>(parts, options);
    if (!opened.volume) {
      return opened;
    }
  }
  if (!opened.volume->IsVolumeOK()) {
    Log::Error("Cannot get NTFS BPB from boot sector of {}",
               Utf::WideToUtf8(parts.source));
    opened.volume.reset();
  }
  return opened;
}

template OpenedVolume<Cache::Strategy::FullCache>
    OpenVolume<Cache::Strategy::FullCache>(const TargetParts&,
                                           const VolumeOptions&);
template OpenedVolume<Cache::Strategy::NoCache>
    OpenVolume<Cache::Strategy::NoCache>(const TargetParts&,
                                         const VolumeOptions&);

}  // namespace NTFS_BROWSER_NAMESPACE::Search
