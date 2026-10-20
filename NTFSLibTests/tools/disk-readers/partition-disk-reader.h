#pragma once

#include <ntfs-browser/win-types.h>

#include <fstream>
#include <optional>
#include <string_view>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

// A real IDiskReader over a whole-disk image file (MBR plus one NTFS
// partition), offsetting every read by partitionOffset. NtfsVolume's own
// path-based constructor expects addr 0 to already be the volume's boot
// sector, which a whole-disk image's byte 0 (the MBR) is not.
class PartitionDiskReader : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  // partition_length is the partition's size in bytes, when the test knows
  // it. The disk's size minus the offset would be too large if unallocated
  // space follows the partition.
  explicit PartitionDiskReader(
      ULONGLONG partition_offset,
      std::optional<ULONGLONG> partition_length = std::nullopt);

  bool Open(std::wstring_view path) override;

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

  // partition_length, or nullopt when the constructor got none.
  [[nodiscard]] std::optional<ULONGLONG> Size() const override;

 private:
  ULONGLONG partition_offset_;
  std::optional<ULONGLONG> partition_length_;
  mutable std::ifstream file_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
