#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "data/header-non-resident.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Test::DiskReaders {

// The sector size the boot sector declares. The update sequence words of a
// record sit every 512 bytes, whatever the sector size, and every kept
// fixture comes from a volume with 512-byte sectors.
inline constexpr DWORD mft_sector_size = 512;

// Reads the $DATA of record 0 of a $MFT and works out where the volume it came
// from placed each run, and how big its clusters were. Record 0 is a record
// whose update sequence is undone already.
class MftRunDecoder {
 public:
  // One run of $MFT's $DATA, as the volume it came from placed it.
  struct Run {
    // First cluster on the volume.
    ULONGLONG lcn{0};
    // First byte of the run in the file.
    ULONGLONG file_offset{0};
    ULONGLONG clusters{0};
  };

  // What Decode() learns from record 0.
  struct Layout {
    std::vector<Run> runs;
    DWORD cluster_size{0};
    // First byte past the last mapped cluster.
    ULONGLONG volume_bytes{0};
    // Bytes of the volume that hold the boot sector and its zero padding.
    ULONGLONG boot_area_bytes{0};
    // The declared extent of record 0 was repaired in the record given to
    // Decode(): its update sequence must be put back.
    bool repaired{false};
  };

  // With repair_declared_extent, Decode() makes a $DATA that declares fewer
  // clusters than its run list holds declare them all. The library rejects such
  // a record unless it recovers, and then keeps the declared count.
  explicit MftRunDecoder(bool repair_declared_extent) noexcept
      : repair_declared_extent_(repair_declared_extent) {}

  // Reads the unnamed $DATA at VCN 0 of record, which maps the whole $MFT, and
  // derives the cluster size from it. Nothing, with Failure() set, when the
  // runs are unusable. record may be changed to repair the declared extent.
  [[nodiscard]] std::optional<Layout> Decode(std::vector<BYTE>& record);

  // Why the last Decode() failed.
  [[nodiscard]] const std::string& Failure() const noexcept;

 private:
  // One run of the run list as record 0 stores it, before the cluster size is
  // known. A sparse run has no lcn.
  struct RawRun {
    std::optional<ULONGLONG> lcn;
    ULONGLONG clusters;
  };

  // The decoded run list of $MFT's $DATA.
  struct RunList {
    std::vector<RawRun> runs;
    ULONGLONG total_clusters{0};
  };

  // $MFT's unnamed $DATA header in record 0, and where it sits in the record.
  struct DataAttr {
    NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident header;
    // Offset of the attribute in the record.
    size_t pos;
    // Offset of the first byte past the attribute.
    size_t end;
  };

  // One entry of a run list: the length, the signed offset from the last real
  // run, and how many bytes the entry takes.
  struct RunEntry {
    ULONGLONG length;
    ULONGLONG delta;
    unsigned offset_bytes;
    size_t size;
  };

  // Sets failure_ and returns false.
  bool Fail(std::string reason);

  // Finds the non-resident unnamed $DATA that starts at VCN 0, and checks that
  // its run list starts inside the attribute.
  std::optional<DataAttr> FindUnnamedData(const std::vector<BYTE>& record);

  // Walks the attributes of record for the non-resident unnamed $DATA that
  // starts at VCN 0.
  std::optional<DataAttr> ScanForData(const std::vector<BYTE>& record);

  // Decodes the entry that starts at rest[0]: the header byte, then the length
  // and the offset. Nothing when a field is cut or the length is zero.
  std::optional<RunEntry> ReadRunEntry(std::span<const BYTE> rest);

  // Decodes the run list of data. The first run MUST be real.
  std::optional<RunList> ParseRunList(const std::vector<BYTE>& record,
                                      const DataAttr& data);

  // When asked to, makes data declare the last VCN its run list reaches, in
  // record and in data. Tells whether it did.
  bool RepairDeclaredExtent(std::vector<BYTE>& record, DataAttr& data,
                            ULONGLONG total_clusters) const;

  // The cluster size that makes the allocated size of data a whole cluster
  // count. Nothing when it is not a plausible size.
  std::optional<ULONGLONG> DeriveClusterSize(
      const NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident& data,
      ULONGLONG total_clusters);

  // Places the real runs of raw_runs in the file, and measures the volume they
  // span. Fills layout.runs, layout.volume_bytes and layout.boot_area_bytes.
  bool BuildRuns(const std::vector<RawRun>& raw_runs, ULONGLONG cluster,
                 Layout& layout);

  bool repair_declared_extent_;
  std::string failure_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Test::DiskReaders
