#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/cache/view.h>
#include <ntfs-browser/namespace.h>

#include "attr/run-list.h"
#include "internal-export.h"
#include "ntfs-browser/alloc/types.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {
namespace Data {

struct RunEntry;

}  // namespace Data

namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {

namespace Attr {

struct HeaderCommon;

}  // namespace Attr

namespace Data {

struct HeaderNonResident;

}  // namespace Data

namespace Efs {

class Context;

}  // namespace Efs

////////////////////////////////
// NonResident Attributes
////////////////////////////////
namespace Attr {

template <Cache::Strategy S>
class AttrNonResident : public Attr::AttrBase<S> {
 public:
  AttrNonResident(const HeaderCommon& ahc,
                  const Io::FileRecord<S>& file_record);
  AttrNonResident(AttrNonResident&& other) noexcept = delete;
  AttrNonResident(const AttrNonResident& other) = delete;
  AttrNonResident& operator=(AttrNonResident&& other) noexcept = delete;
  AttrNonResident& operator=(const AttrNonResident& other) = delete;
  ~AttrNonResident() override = default;

  // Makes ReadData() decrypt this stream with the given context.
  void SetEfsContext(std::shared_ptr<const Efs::Context> context) noexcept;

  [[nodiscard]] ULONGLONG GetStartVcn() const noexcept;
  [[nodiscard]] ULONGLONG GetLastVcn() const noexcept;
  [[nodiscard]] ULONGLONG MappedClusters() const noexcept;
  // Virtual clusters this attribute describes, merged instances included.
  [[nodiscard]] ULONGLONG TotalClusters() const noexcept;

  // Splices other's own runs onto this instance's, as the next VCN range.
  void AppendRuns(const AttrNonResident& other);

 private:
  const Data::HeaderNonResident& attr_header_nr_;
  // Decoded data runs, with this instance's own VCN count.
  RunList runs_;

  // Clusters per compression unit (2^comp_unit_size); 0 means uncompressed.
  ULONGLONG comp_unit_clusters_{0};

  // Decompressed compression units, keyed by unit index; lifetime follows
  // Strategy (FullCache keeps them, NoCache clears per ReadData()).
  mutable std::unordered_map<ULONGLONG, std::vector<BYTE>> comp_unit_cache_;

  // Set only on an encrypted stream. Shared with the record's other encrypted
  // streams, which hold the same key.
  std::shared_ptr<const Efs::Context> efs_context_;

  // Whether the record this attribute sits in was freed, and its record
  // number. The cluster check needs both: a freed record legitimately points
  // at free clusters, and the volume's own metadata is not checked.
  bool record_deleted_{false};
  std::optional<ULONGLONG> record_number_;
  // Set by the first ReadData(): the check runs once per attribute.
  mutable bool alloc_checked_{false};

  void ParseDataRun();
  // With recover_errors on, logs once if a cluster of this live data stream
  // is free in the cluster bitmap, and once if one is marked bad in $BadClus.
  // Never changes what a read returns.
  void CheckClusterAllocation() const;
  [[nodiscard]] std::optional<Cache::ReadResult<S>>
      ReadClusters(ULONGLONG clusters, ULONGLONG start_lcn,
                   ULONGLONG offset) const;
  [[nodiscard]] bool ReadClusterPart(ULONGLONG vcn, DWORD from,
                                     std::span<BYTE> out) const;
  [[nodiscard]] std::optional<ULONGLONG>
      ReadLeadingPart(ULONGLONG offset, std::span<BYTE> out) const;
  [[nodiscard]] bool ReadFromClusterStart(ULONGLONG vcn,
                                          std::span<BYTE> out) const;
  [[nodiscard]] std::optional<ULONGLONG>
      ReadDataBounded(ULONGLONG offset, const std::span<BYTE>& buffer,
                      ULONGLONG limit) const;
  [[nodiscard]] std::optional<ULONGLONG>
      ReadVirtualClusters(ULONGLONG vcn, ULONGLONG clusters,
                          std::span<BYTE> buffer) const;
  [[nodiscard]] std::optional<ULONGLONG>
      ReadVirtualClustersRaw(ULONGLONG vcn, ULONGLONG clusters,
                             std::span<BYTE> buffer) const;

  // Compression support. See attr-non-resident.cpp for the compression-unit
  // layout these implement.
  [[nodiscard]] ULONGLONG UnitClusters(ULONGLONG unit_first_vcn) const noexcept;
  [[nodiscard]] const std::vector<BYTE>*
      GetCompressionUnit(ULONGLONG unit_index) const;
  // How reading one data run's clusters ended.
  enum class RunRead : BYTE {
    Done,       // Clusters read (or zero-filled, for a sparse run).
    ShortRead,  // The disk read failed: the caller keeps what it has.
    Failed      // Decryption failed: the whole read fails.
  };

  [[nodiscard]] RunRead ReadRunClusters(const Data::RunEntry& data_run,
                                        ULONGLONG vcn,
                                        ULONGLONG clusters_to_read,
                                        std::span<BYTE> out) const;
  [[nodiscard]] bool DecompressUnit(ULONGLONG unit_index,
                                    ULONGLONG unit_first_vcn,
                                    ULONGLONG real_clusters,
                                    std::vector<BYTE>& unit) const;
  [[nodiscard]] bool FillCompressionUnit(ULONGLONG unit_index,
                                         ULONGLONG unit_first_vcn,
                                         ULONGLONG unit_clusters,
                                         ULONGLONG real_clusters,
                                         std::vector<BYTE>& unit) const;
  [[nodiscard]] std::optional<ULONGLONG>
      ReadVirtualClustersCompressed(ULONGLONG vcn, ULONGLONG clusters,
                                    std::span<BYTE> buffer) const;

 public:
  [[nodiscard]] const BYTE* GetData() const noexcept override;
  [[nodiscard]] ULONGLONG GetDataSize() const noexcept override;
  [[nodiscard]] ULONGLONG GetAllocatedSize() const noexcept override;
  [[nodiscard]] std::optional<ULONGLONG>
      ReadData(ULONGLONG offset, const std::span<BYTE>& buffer) const override;
  [[nodiscard]] Alloc::StreamExtents GetExtents() const override;
  [[nodiscard]] Alloc::StreamRanges GetRealRanges() const override;
  [[nodiscard]] Alloc::StreamLayout GetLayout() const override;

  // Like ReadData(), but bounded by this instance's own VCN range instead
  // of real_size, which continuation instances leave at 0. Public only for
  // the unit tests.
  [[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::optional<ULONGLONG>
      ReadExtentData(ULONGLONG offset, const std::span<BYTE>& buffer) const;

  // Announces to the volume the clusters a ReadExtentData() of length bytes at
  // offset reads, run by run. Reads nothing. A compressed stream reads whole
  // units, and announces nothing.
  void PrefetchExtentData(ULONGLONG offset, ULONGLONG length) const;
};  // AttrNonResident

}  // namespace Attr

}  // namespace NTFS_BROWSER_NAMESPACE
