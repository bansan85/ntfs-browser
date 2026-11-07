#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/tool/device-access/bench-options.h>
#include <ntfs-browser/tool/device-access/device-path.h>
#include <ntfs-browser/tool/device-access/text-convert.h>
#include <ntfs-browser/tool/device-access/tool-support.h>
#include <ntfs-browser/tool/device-access/volume-target.h>
#include <ntfs-browser/volume-options.h>

#ifndef _WIN32
  #include <ntfs-browser/tool/device-access/posix-device-disk-reader.h>
#endif

namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess {

namespace {

// Reports whether character separates the parts of a path inside a volume.
bool IsInsideSeparator(wchar_t character) {
  return character == L'\\' || character == L'/';
}

// Joins the parts of a path with the separator the volume code walks on.
std::wstring JoinInsidePath(std::wstring_view path) {
  std::wstring joined;
  for (const std::wstring_view part : SplitInsidePath(path)) {
    if (!joined.empty()) {
      joined += L'\\';
    }
    joined += part;
  }
  return joined;
}

// Opens the image file or device at device. A null volume says why in error.
template <Cache::Strategy S>
OpenedVolume<S> OpenDevice(const std::filesystem::path& device,
                           const VolumeOptions& options) {
  OpenedVolume<S> opened;
  opened.source = device;
  const std::wstring label = WideFromPath(device);
#ifdef _WIN32
  // Win32DiskReader opens a plain file the same way as a device.
  opened.volume =
      std::make_unique<NtfsVolume<S>>(std::wstring_view(label), options);
#else
  auto reader = std::make_unique<PosixDeviceDiskReader>();
  if (!reader->Open(label)) {
    const int error = reader->LastError();
    opened.error =
        error == EACCES || error == EPERM
            ? L"Cannot read device " + label + L": read access is needed"
            : L"Cannot open " + label;
    return opened;
  }
  opened.volume = std::make_unique<NtfsVolume<S>>(std::move(reader), options);
#endif
  if (!opened.volume->IsVolumeOK()) {
    opened.volume.reset();
    opened.error = L"Cannot get NTFS BPB from boot sector of " + label;
  }
  return opened;
}

#ifdef _WIN32

// Reports whether character is a letter of the ASCII alphabet.
bool IsAsciiLetter(wchar_t character) {
  return (character >= L'a' && character <= L'z') ||
         (character >= L'A' && character <= L'Z');
}

// Opens the drive that target starts with, in the form "c:" or "c:\dir\file".
// A null volume says why in error. Without that form, error stays empty.
template <Cache::Strategy S>
OpenedVolume<S> OpenDrive(std::wstring_view target,
                          const VolumeOptions& options) {
  OpenedVolume<S> opened;
  if (target.size() < 2 || !IsAsciiLetter(target[0]) || target[1] != L':' ||
      (target.size() > 2 && !IsInsideSeparator(target[2]))) {
    return opened;
  }

  opened.inside = JoinInsidePath(target.substr(2));
  opened.source = std::wstring{target[0], L':', L'\\'};
  opened.volume = std::make_unique<NtfsVolume<S>>(target[0], options);
  if (!opened.volume->IsVolumeOK()) {
    opened.volume.reset();
    opened.error = L"Cannot get NTFS BPB from boot sector of volume " +
                   std::wstring(1, target[0]);
  }
  return opened;
}

#endif  // _WIN32

}  // namespace

OptionResult ParseCommonOption(std::wstring_view arg, CommonArguments& args) {
  if (arg == L"--help" || arg == L"-h") {
    args.help = true;
  } else if (IsLogOption(arg)) {
    return ParseLogOption(arg, args.log_config) ? OptionResult::Accepted
                                                : OptionResult::Malformed;
  } else if (arg == L"--deleted") {
    args.volume_options.include_deleted = true;
  } else if (arg == L"--recover") {
    args.volume_options.recover_errors = true;
  } else {
    return OptionResult::NotMine;
  }
  return OptionResult::Accepted;
}

std::string CommonOptionsUsage() {
  std::string usage;
  usage += "  --deleted        also find deleted files\n";
  usage += "  --recover        salvage damaged directory and file data\n";
  usage += "  ";
  usage += std::string(Log::option_usage);
  usage += "\n";
  usage += "  --help, -h       print this help\n";
  return usage;
}

std::vector<std::wstring_view> SplitInsidePath(std::wstring_view path) {
  std::vector<std::wstring_view> parts;
  std::size_t start = 0;
  while (start <= path.size()) {
    std::size_t end = start;
    while (end < path.size() && !IsInsideSeparator(path[end])) {
      ++end;
    }
    if (end > start) {
      parts.push_back(path.substr(start, end - start));
    }
    start = end + 1;
  }
  return parts;
}

template <Cache::Strategy S>
OpenedVolume<S> OpenVolumeFromTarget(std::wstring_view target,
                                     const VolumeOptions& options) {
  OpenedVolume<S> opened;
  if (const std::optional<DeviceTarget> split = SplitDeviceTarget(target)) {
    opened = OpenDevice<S>(split->device, options);
    opened.inside = JoinInsidePath(split->directory);
  }

#ifdef _WIN32
  // "c:\dir\file" can also name an existing file of the host. That file is no
  // image, so the drive is tried.
  if (!opened.volume) {
    OpenedVolume<S> drive = OpenDrive<S>(target, options);
    if (drive.volume || !drive.error.empty()) {
      return drive;
    }
  }
#endif

  if (!opened.volume && opened.error.empty()) {
    opened.error =
        L"Cannot find a device or image file in " + std::wstring(target);
  }
  return opened;
}

template OpenedVolume<Cache::Strategy::FullCache>
    OpenVolumeFromTarget<Cache::Strategy::FullCache>(std::wstring_view,
                                                     const VolumeOptions&);
template OpenedVolume<Cache::Strategy::NoCache>
    OpenVolumeFromTarget<Cache::Strategy::NoCache>(std::wstring_view,
                                                   const VolumeOptions&);

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess
