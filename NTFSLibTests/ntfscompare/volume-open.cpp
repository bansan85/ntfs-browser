#include "volume-open.h"

#include <cstdio>
#include <system_error>
#include <utility>

#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

#ifdef _WIN32

namespace NtfsCompare
{

std::optional<VolumeHandles> OpenVolumeFor(const std::filesystem::path& target)
{
  std::error_code ec;
  const std::filesystem::path canonical =
      std::filesystem::weakly_canonical(target, ec);
  if (ec || canonical.root_name().empty())
  {
    std::fprintf(stderr, "Cannot determine a drive letter for %ls\n",
                 target.c_str());
    return std::nullopt;
  }
  const wchar_t driveLetter = canonical.root_name().wstring().front();

  VolumeHandles handles;
  handles.full_cache =
      std::make_unique<NtfsVolume<Strategy::FULL_CACHE>>(driveLetter);
  handles.no_cache =
      std::make_unique<NtfsVolume<Strategy::NO_CACHE>>(driveLetter);
  if (!handles.full_cache->IsVolumeOK() || !handles.no_cache->IsVolumeOK())
  {
    std::fprintf(stderr,
                 "Cannot open %lc: as an NTFS volume (running as "
                 "Administrator may be required)\n",
                 driveLetter);
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

namespace NtfsCompare
{

namespace
{

// Capacity of the getmntent_r() line buffer: one page, which holds any
// realistic /proc/mounts line.
constexpr size_t kMountLineBufferSize = 4096;

// The mount point whose path is the longest prefix of target, and its
// device/source. std::nullopt if /proc/mounts lists nothing target sits
// under (should not happen for a real path).
struct MountInfo
{
  std::filesystem::path mount_point;
  std::string device;
};

std::optional<MountInfo> FindMount(const std::filesystem::path& target)
{
  FILE* mounts = setmntent("/proc/mounts", "r");
  if (mounts == nullptr)
  {
    return std::nullopt;
  }

  const std::string targetStr = target.string();
  std::optional<MountInfo> best;
  size_t bestLength = 0;
  struct mntent entry = {};
  char buffer[kMountLineBufferSize];
  while (getmntent_r(mounts, &entry, buffer, sizeof(buffer)) != nullptr)
  {
    const std::string mountStr = entry.mnt_dir;
    const bool isPrefix =
        targetStr.compare(0, mountStr.size(), mountStr) == 0 &&
        (targetStr.size() == mountStr.size() ||
         targetStr[mountStr.size()] == '/');
    if (isPrefix && mountStr.size() >= bestLength)
    {
      bestLength = mountStr.size();
      best =
          MountInfo{.mount_point = entry.mnt_dir, .device = entry.mnt_fsname};
    }
  }
  endmntent(mounts);
  return best;
}

}  // namespace

std::optional<VolumeHandles> OpenVolumeFor(const std::filesystem::path& target)
{
  std::error_code error_code;
  const std::filesystem::path canonical =
      std::filesystem::weakly_canonical(target, error_code);
  if (error_code)
  {
    std::fprintf(stderr, "Cannot resolve %s\n", target.c_str());
    return std::nullopt;
  }

  const std::optional<MountInfo> mount = FindMount(canonical);
  if (!mount)
  {
    std::fprintf(stderr, "Cannot find the mount point backing %s\n",
                 canonical.c_str());
    return std::nullopt;
  }
  if (!mount->device.starts_with("/dev/"))
  {
    std::fprintf(stderr,
                 "%s is not backed by a real block device (mounted from "
                 "\"%s\")\n",
                 canonical.c_str(), mount->device.c_str());
    return std::nullopt;
  }
  const std::wstring devicePath(mount->device.begin(), mount->device.end());

  auto fullCacheReader = std::make_unique<RawDeviceDiskReader>();
  auto noCacheReader = std::make_unique<RawDeviceDiskReader>();
  const bool opened =
      fullCacheReader->Open(devicePath) && noCacheReader->Open(devicePath);
  if (!opened)
  {
    std::fprintf(stderr, "Cannot open %s (root privileges may be required)\n",
                 mount->device.c_str());
    return std::nullopt;
  }

  VolumeHandles handles;
  handles.full_cache = std::make_unique<NtfsVolume<Strategy::FULL_CACHE>>(
      std::move(fullCacheReader));
  handles.no_cache = std::make_unique<NtfsVolume<Strategy::NO_CACHE>>(
      std::move(noCacheReader));
  if (!handles.full_cache->IsVolumeOK() || !handles.no_cache->IsVolumeOK())
  {
    std::fprintf(stderr, "%s is not an NTFS volume\n", mount->device.c_str());
    return std::nullopt;
  }

  handles.relative_path =
      canonical.lexically_relative(mount->mount_point).generic_wstring();
  return handles;
}

}  // namespace NtfsCompare

#endif  // _WIN32
