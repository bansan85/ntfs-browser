#include "volume-open.h"

#include <array>
#include <system_error>
#include <utility>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/ntfs-volume.h>

#include "console.h"

using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;

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
  handles.full_cache =
      std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(drive_letter);
  handles.no_cache =
      std::make_unique<NtfsVolume<Cache::Strategy::NoCache>>(drive_letter);
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

  #include <mntent.h>

  #include "raw-device-disk-reader.h"

namespace NtfsCompare {

namespace {

// Capacity of the getmntent_r() line buffer: one page, which holds any
// realistic /proc/mounts line.
constexpr size_t mount_line_buffer_size = 4096;

// The mount point whose path is the longest prefix of target, and its
// device/source. std::nullopt if /proc/mounts lists nothing target sits
// under (should not happen for a real path).
struct MountInfo {
  std::filesystem::path mount_point;
  std::string device;
};

std::optional<MountInfo> FindMount(const std::filesystem::path& target) {
  FILE* mounts = setmntent("/proc/mounts", "r");
  if (mounts == nullptr) {
    return std::nullopt;
  }

  const std::string target_str = target.string();
  std::optional<MountInfo> best;
  size_t best_length = 0;
  struct mntent entry = {};
  std::array<char, mount_line_buffer_size> buffer{};
  while (getmntent_r(mounts, &entry, buffer.data(), buffer.size()) != nullptr) {
    const std::string mount_str = entry.mnt_dir;
    const bool is_prefix =
        target_str.starts_with(mount_str) &&
        (target_str.size() == mount_str.size() ||
         // The size test just above is false, so targetStr is longer.
         // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
         target_str[mount_str.size()] == '/');
    if (is_prefix && mount_str.size() >= best_length) {
      best_length = mount_str.size();
      best =
          MountInfo{.mount_point = entry.mnt_dir, .device = entry.mnt_fsname};
    }
  }
  endmntent(mounts);
  return best;
}

}  // namespace

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
  const std::wstring device_path(mount->device.begin(), mount->device.end());

  auto full_cache_reader = std::make_unique<RawDeviceDiskReader>();
  auto no_cache_reader = std::make_unique<RawDeviceDiskReader>();
  const bool opened = full_cache_reader->Open(device_path) &&
                      no_cache_reader->Open(device_path);
  if (!opened) {
    PrintErr("Cannot open {} (root privileges may be required)\n",
             mount->device);
    return std::nullopt;
  }

  VolumeHandles handles;
  handles.full_cache = std::make_unique<NtfsVolume<Cache::Strategy::FullCache>>(
      std::move(full_cache_reader));
  handles.no_cache = std::make_unique<NtfsVolume<Cache::Strategy::NoCache>>(
      std::move(no_cache_reader));
  if (!handles.full_cache->IsVolumeOK() || !handles.no_cache->IsVolumeOK()) {
    PrintErr("{} is not an NTFS volume\n", mount->device);
    return std::nullopt;
  }

  handles.relative_path =
      canonical.lexically_relative(mount->mount_point).generic_wstring();
  return handles;
}

}  // namespace NtfsCompare

#endif  // _WIN32
