#include "volume-open.h"

#include <array>
#include <system_error>
#include <utility>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "console.h"

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

#ifdef _WIN32

namespace NtfsCompare {

std::optional<VolumeHandles>
    OpenVolumeFor(const std::filesystem::path& target) {
  std::error_code ec;
  const std::filesystem::path canonical =
      std::filesystem::weakly_canonical(target, ec);
  if (ec || canonical.root_name().empty()) {
    PrintErr("Cannot determine a drive letter for {}\n", NativeText(target));
    return std::nullopt;
  }
  const wchar_t drive_letter = canonical.root_name().wstring().front();

  VolumeHandles handles;
  handles.full_cache = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      drive_letter, comparison_volume_options);
  handles.no_cache = std::make_unique<NtfsVolume<Cache::Strategy::NoCache>>(
      drive_letter, comparison_volume_options);
  if (!handles.full_cache->IsVolumeOK() || !handles.no_cache->IsVolumeOK()) {
    // A drive letter is always ASCII.
    PrintErr(
        "Cannot open {}: as an NTFS volume (running as "
        "Administrator may be required)\n",
        static_cast<char>(drive_letter));
    return std::nullopt;
  }

  handles.relative_path =
      canonical.lexically_relative(canonical.root_path()).generic_wstring();
  return handles;
}

}  // namespace NtfsCompare

#else  // POSIX

  #include <algorithm>
  #include <cerrno>
  #include <cstdio>
  #include <string>
  #include <vector>

  #include <mntent.h>

  #include "posix-device-disk-reader.h"
  #include "text-convert.h"

namespace NtfsCompare {

namespace {

// Capacity of the getmntent_r() line buffer: one page, which holds any
// realistic /proc/mounts line.
constexpr size_t mount_line_buffer_size = 4096;

// One line of a mount table: the mount point, the source (device) and the
// filesystem type.
struct MountInfo {
  std::filesystem::path mount_point;
  std::string device;
  std::string fstype;
};

// Every entry of mounts_file, in file order. Empty when it cannot be read.
std::vector<MountInfo> ReadMounts(const char* mounts_file) {
  std::vector<MountInfo> mounts;
  FILE* const table = setmntent(mounts_file, "r");
  if (table == nullptr) {
    return mounts;
  }

  struct mntent entry = {};
  std::array<char, mount_line_buffer_size> buffer{};
  while (getmntent_r(table, &entry, buffer.data(), buffer.size()) != nullptr) {
    mounts.push_back({.mount_point = entry.mnt_dir,
                      .device = entry.mnt_fsname,
                      .fstype = entry.mnt_type});
  }
  endmntent(table);
  return mounts;
}

// The mount point whose path is the longest prefix of target, and its
// device/source. std::nullopt if /proc/mounts lists nothing target sits
// under (should not happen for a real path).
std::optional<MountInfo> FindMount(const std::filesystem::path& target) {
  const std::string target_str = target.string();
  std::optional<MountInfo> best;
  size_t best_length = 0;
  for (const MountInfo& mount : ReadMounts("/proc/mounts")) {
    const std::string mount_str = mount.mount_point.string();
    const bool is_prefix =
        target_str.starts_with(mount_str) &&
        (target_str.size() == mount_str.size() ||
         // The size test just above is false, so targetStr is longer.
         // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
         target_str[mount_str.size()] == '/');
    if (is_prefix && mount_str.size() >= best_length) {
      best_length = mount_str.size();
      best = mount;
    }
  }
  return best;
}

// The two readers over device, one for each cache strategy's volume.
struct ReaderPair {
  std::unique_ptr<NtfsDevice::PosixDeviceDiskReader> full_cache;
  std::unique_ptr<NtfsDevice::PosixDeviceDiskReader> no_cache;
};

// Opens device twice. Returns nothing after saying why on stderr when it
// cannot, telling a missing read permission from any other failure.
std::optional<ReaderPair> OpenReaders(const std::filesystem::path& device) {
  const std::wstring device_wide = NtfsDevice::WideFromPath(device);
  ReaderPair readers{
      .full_cache = std::make_unique<NtfsDevice::PosixDeviceDiskReader>(),
      .no_cache = std::make_unique<NtfsDevice::PosixDeviceDiskReader>()};
  for (NtfsDevice::PosixDeviceDiskReader* reader :
       {readers.full_cache.get(), readers.no_cache.get()}) {
    if (!reader->Open(device_wide)) {
      const int error = reader->LastError();
      if (error == EACCES || error == EPERM) {
        PrintErr("Cannot read device {}: read access is needed\n",
                 NativeText(device));
      } else {
        PrintErr("Cannot open {}\n", NativeText(device));
      }
      return std::nullopt;
    }
  }
  return readers;
}

// Builds both volumes over readers. Returns nothing after saying so on
// stderr when device holds no NTFS volume.
std::optional<VolumeHandles> OpenVolumes(ReaderPair readers,
                                         const std::filesystem::path& device) {
  VolumeHandles handles;
  handles.full_cache = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      std::move(readers.full_cache), comparison_volume_options);
  handles.no_cache = std::make_unique<NtfsVolume<Cache::Strategy::NoCache>>(
      std::move(readers.no_cache), comparison_volume_options);
  if (!handles.full_cache->IsVolumeOK() || !handles.no_cache->IsVolumeOK()) {
    PrintErr("{} is not an NTFS volume\n", NativeText(device));
    return std::nullopt;
  }
  return handles;
}

}  // namespace

std::optional<MountEntry> FindMountOfDevice(const std::filesystem::path& device,
                                            const char* mounts_file) {
  std::error_code error_code;
  const std::filesystem::path wanted =
      std::filesystem::weakly_canonical(device, error_code);
  if (error_code) {
    return std::nullopt;
  }

  // The last match is the mount on top, when a device is mounted twice.
  std::optional<MountEntry> found;
  for (const MountInfo& mount : ReadMounts(mounts_file)) {
    const std::filesystem::path source =
        std::filesystem::weakly_canonical(mount.device, error_code);
    if (!error_code && source == wanted) {
      found =
          MountEntry{.mount_point = mount.mount_point, .fstype = mount.fstype};
    }
  }
  return found;
}

std::optional<VolumeHandles>
    OpenDeviceVolume(const NtfsDevice::DeviceTarget& target) {
  std::optional<ReaderPair> readers = OpenReaders(target.device);
  if (!readers) {
    return std::nullopt;
  }
  std::optional<VolumeHandles> handles =
      OpenVolumes(std::move(*readers), target.device);
  if (!handles) {
    return std::nullopt;
  }

  // This tool keys paths with '/', and a device target uses '\'.
  handles->relative_path = target.directory;
  std::ranges::replace(handles->relative_path, L'\\', L'/');

  if (const std::optional<MountEntry> mount =
          FindMountOfDevice(target.device)) {
    handles->mount_point = mount->mount_point;
    handles->mount_fstype = mount->fstype;
  }
  return handles;
}

std::optional<VolumeHandles>
    OpenVolumeFor(const std::filesystem::path& target) {
  std::error_code error_code;
  const std::filesystem::path canonical =
      std::filesystem::weakly_canonical(target, error_code);
  if (error_code) {
    PrintErr("Cannot resolve {}\n", NativeText(target));
    return std::nullopt;
  }

  const std::optional<MountInfo> mount = FindMount(canonical);
  if (!mount) {
    PrintErr("Cannot find the mount point backing {}\n", NativeText(canonical));
    return std::nullopt;
  }
  if (!mount->device.starts_with("/dev/")) {
    PrintErr(
        "{} is not backed by a real block device (mounted from "
        "\"{}\")\n",
        NativeText(canonical), mount->device);
    return std::nullopt;
  }

  std::optional<ReaderPair> readers = OpenReaders(mount->device);
  if (!readers) {
    return std::nullopt;
  }
  std::optional<VolumeHandles> handles =
      OpenVolumes(std::move(*readers), mount->device);
  if (!handles) {
    return std::nullopt;
  }

  handles->relative_path = NtfsDevice::WideFromPath(
      canonical.lexically_relative(mount->mount_point).generic_string());
  handles->mount_point = mount->mount_point;
  handles->mount_fstype = mount->fstype;
  return handles;
}

}  // namespace NtfsCompare

#endif  // _WIN32
