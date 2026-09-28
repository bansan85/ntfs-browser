#pragma once

#include <ntfs-browser/win-types.h>

#include <fstream>
#include <span>
#include <string_view>

#include <ntfs-browser/disk-reader.h>

namespace NtfsBrowserTests
{

// A real IDiskReader over a whole-disk image file (MBR plus one NTFS
// partition), offsetting every read by partitionOffset. NtfsVolume's own
// path-based constructor expects addr 0 to already be the volume's boot
// sector, which a whole-disk image's byte 0 (the MBR) is not.
class PartitionDiskReader : public NtfsBrowser::IDiskReader
{
 public:
  explicit PartitionDiskReader(ULONGLONG partitionOffset);

  bool Open(std::wstring_view path) override;

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

 private:
  ULONGLONG partition_offset_;
  mutable std::ifstream file_;
};

}  // namespace NtfsBrowserTests
