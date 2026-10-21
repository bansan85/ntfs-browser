#pragma once

#include <ntfs-browser/win-types.h>

#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

// Presents a standalone $MFT file, with no disk image around it, as the $MFT
// of a volume. Byte 0 is a boot sector the reader builds. Every cluster of
// $MFT's unnamed $DATA, as record 0 maps it, serves the matching bytes of the
// file. A file that ends inside a cluster reads as zeros up to the end of that
// cluster, since a volume holds whole clusters. Any other address fails to
// read: a $MFT never holds the clusters of the other files, and a zero fill
// would pass for a $MFT bitmap that marks every record free.
//
// The file is read on demand, never copied: the largest kept $MFT is 120 MB.
//
// Both sizes come from the file. The record size is the allocated size in the
// header of record 0. The cluster size is the allocated size of $MFT's $DATA
// over its cluster count. The volume's metadata (a $Bitmap, a $UpCase, an
// $Extend) is not in the file: the volume opens without it.
class StandaloneMftDiskReader : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  // Asks the next Open() to serve a record 0 whose declared last VCN covers
  // every cluster of its run list, when the run list is longer. The xw fixture
  // is one: its $DATA declares 298 clusters and holds runs for 314. The
  // library rejects such a record unless it recovers, and then keeps 298. The
  // file on disk is never changed: the reader overlays its copy of record 0.
  void SetRepairDeclaredExtent(bool repair) noexcept;

  // Opens path and reads its record 0. False, with Failure() set, when record
  // 0 is not a usable first $MFT record.
  bool Open(std::wstring_view path) override;

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

  // The end of the last cluster $MFT maps: the volume's size in bytes. Empty
  // before a successful Open().
  [[nodiscard]] std::optional<ULONGLONG> Size() const override;

  // Why the last Open() failed. Empty after a success.
  [[nodiscard]] const std::string& Failure() const noexcept;

  // Size in bytes of one file record, from record 0's header.
  [[nodiscard]] DWORD FileRecordSize() const noexcept;

  // Size in bytes of one cluster, from record 0's $DATA.
  [[nodiscard]] DWORD ClusterSize() const noexcept;

  // Size in bytes of the file on disk.
  [[nodiscard]] ULONGLONG FileSize() const noexcept;

 private:
  // One run of $MFT's $DATA, as the volume it came from placed it.
  struct Run {
    // First cluster on the volume.
    ULONGLONG lcn{0};
    // First byte of the run in the file.
    ULONGLONG file_offset{0};
    ULONGLONG clusters{0};
  };

  // Sets failure_ and returns false.
  bool Fail(std::string reason);

  // Decodes record 0's $DATA runs into runs_. False when they are unusable.
  // record has its update sequence undone, and may be changed to repair the
  // declared extent.
  bool MapDataRuns(std::vector<BYTE>& record);

  // Builds boot_ from the geometry found.
  void BuildBootSector();

  // Fills dest from the file at file_offset, zero padded to a whole cluster.
  [[nodiscard]] bool ReadFromFile(ULONGLONG file_offset,
                                  std::span<BYTE> dest) const;

  bool repair_declared_extent_{false};
  // Record 0 as served, update sequence applied, when it was repaired.
  std::vector<BYTE> repaired_record_;
  std::string failure_;
  DWORD file_record_size_{0};
  DWORD cluster_size_{0};
  ULONGLONG file_size_{0};
  // First byte past the last mapped cluster.
  ULONGLONG volume_bytes_{0};
  // Bytes of the volume that hold the boot sector and its zero padding.
  ULONGLONG boot_area_bytes_{0};
  std::vector<Run> runs_;
  std::vector<BYTE> boot_;
  mutable std::ifstream file_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
