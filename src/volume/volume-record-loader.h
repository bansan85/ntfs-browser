#pragma once

#include <cstdint>

#include <ntfs-browser/namespace.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

template <Cache::Strategy S>
class NtfsVolume;

namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io

namespace Volume {

// What $Volume says about the volume.
struct VolumeRecordParse {
  // True when the record parsed and the NTFS version is 3.0 or later.
  bool ok{false};
  // The NTFS version. Zero when the record did not parse, and set even when
  // the version is too old to accept.
  BYTE version_major{0};
  BYTE version_minor{0};
};

// Reads $Volume (MFT record 3): the NTFS version, the dirty flag and the name.
// It logs what it finds.
template <Cache::Strategy S>
class VolumeRecordLoader {
 public:
  // The volume MUST outlive this object.
  explicit VolumeRecordLoader(const NtfsVolume<S>& volume) noexcept;

  // Parses $Volume. A volume older than 3.0, the NT4 era, is not ok.
  [[nodiscard]] VolumeRecordParse Load() const;

 private:
  // Parses VolumeInformation and VolumeName into vol. False when the record
  // does not parse or has no VolumeInformation.
  [[nodiscard]] static bool ParseRecord(Io::FileRecord<S>& vol);

  // Reads the NTFS version from $VOLUME_INFORMATION and logs it, with a warning
  // if the volume is dirty. Needs ParseRecord() to have passed first.
  [[nodiscard]] static VolumeRecordParse
      ReadVersion(const Io::FileRecord<S>& vol);

  // Logs the volume's name, if $Volume has a VolumeName attribute.
  static void LogName(const Io::FileRecord<S>& vol);

  const NtfsVolume<S>* volume_;
};

}  // namespace Volume
}  // namespace NTFS_BROWSER_NAMESPACE
