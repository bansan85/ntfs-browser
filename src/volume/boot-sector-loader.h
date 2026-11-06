#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <optional>

#include <ntfs-browser/cache/view.h>
#include <ntfs-browser/namespace.h>

#include "volume/volume-geometry.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Io {

template <Cache::Strategy S>
class FileReader;

}  // namespace Io

namespace Volume {

// Finds a volume's geometry on disk: reads the boot sector, and the backup copy
// of it when recovering and the first sector is unusable.
template <Cache::Strategy S>
class BootSectorLoader {
 public:
  // The reader MUST outlive this object.
  BootSectorLoader(const Io::FileReader<S>& reader, bool recover) noexcept;

  // Reads and decodes the boot sector. When recovering and that sector is
  // unusable, the backup boot sector replaces it if it parses. Otherwise the
  // result keeps what the first parse decoded, with ok false.
  [[nodiscard]] BootSectorParse Load() const;

 private:
  // Reads the boot sector, retrying with 512 bytes when the 4096-byte read
  // fails.
  [[nodiscard]] std::optional<Cache::ReadResult<S>> ReadBootSector() const;

  // Looks for the copy of the boot sector that NTFS keeps in the last sector of
  // the volume. Only a volume that fills its whole backing store ends there: a
  // partition larger than its file system puts the copy before the end. Each
  // candidate is read apart from the block cache, so that FullCache does not
  // ask for a 64 KiB block that runs past the end. Empty when no candidate
  // fits.
  [[nodiscard]] std::optional<BootSectorParse> ReadBackupBootSector() const;

  // Reads the last sector_size bytes of a backing store of volume_bytes. Empty
  // unless they parse with that same sector size.
  [[nodiscard]] std::optional<BootSectorParse>
      TryBackupSectorSize(ULONGLONG volume_bytes, WORD sector_size) const;

  const Io::FileReader<S>& reader_;
  bool recover_;
};

}  // namespace Volume
}  // namespace NTFS_BROWSER_NAMESPACE
