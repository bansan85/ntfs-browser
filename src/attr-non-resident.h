#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <ntfs-browser/attr-base.h>

#include "data/run-entry.h"

namespace NtfsBrowser {

enum class Strategy : std::uint8_t;
struct AttrHeaderCommon;
template <Strategy S>
class FileRecord;
template <Strategy S>
class NtfsVolume;

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

template <Strategy S>
class AttrNonResident : public AttrBase<S> {
 public:
  AttrNonResident(const AttrHeaderCommon& ahc,
                  const FileRecord<S>& file_record);
  AttrNonResident(AttrNonResident&& other) noexcept = delete;
  AttrNonResident(const AttrNonResident& other) = delete;
  AttrNonResident& operator=(AttrNonResident&& other) noexcept = delete;
  AttrNonResident& operator=(const AttrNonResident& other) = delete;
  ~AttrNonResident() override = default;

  friend class FileRecord<S>;
  friend class NtfsVolume<S>;

 private:
  const Data::HeaderNonResident& attr_header_nr_;
  std::vector<Data::RunEntry> data_run_list_;

  // This instance's own VCN count; AppendRuns() extends it per merged instance.
  ULONGLONG merged_clusters_{0};

  // Clusters per compression unit (2^comp_unit_size); 0 means uncompressed.
  ULONGLONG comp_unit_clusters_{0};

  // Decompressed compression units, keyed by unit index; lifetime follows
  // Strategy (FullCache keeps them, NoCache clears per ReadData()).
  mutable std::unordered_map<ULONGLONG, std::vector<BYTE>> comp_unit_cache_;

  // Set only on an encrypted stream. Shared with the record's other encrypted
  // streams, which hold the same key.
  std::shared_ptr<const Efs::Context> efs_context_;

  [[nodiscard]] static bool PickData(std::span<const BYTE>& data_run,
                                     ULONGLONG& length, LONGLONG& lcn_offset,
                                     bool recover) noexcept;
  void ParseDataRun();
  [[nodiscard]] bool AppendDataRun(ULONGLONG length, LONGLONG lcn_offset,
                                   LONGLONG& lcn, ULONGLONG& vcn, bool recover);
  [[nodiscard]] std::optional<std::span<const BYTE>>
      ReadClusters(ULONGLONG clusters, ULONGLONG start_lcn,
                   ULONGLONG offset) const;
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
  [[nodiscard]] ULONGLONG TotalClusters() const noexcept;
  [[nodiscard]] ULONGLONG UnitClusters(ULONGLONG unit_first_vcn) const noexcept;
  [[nodiscard]] std::optional<ULONGLONG>
      LeadingRealClusters(ULONGLONG unit_first_vcn,
                          ULONGLONG unit_clusters) const noexcept;
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
  [[nodiscard]] std::optional<ULONGLONG>
      ReadVirtualClustersCompressed(ULONGLONG vcn, ULONGLONG clusters,
                                    std::span<BYTE> buffer) const;

  // Makes ReadData() decrypt this stream with the given context.
  void SetEfsContext(std::shared_ptr<const Efs::Context> context) noexcept;

  // Like ReadData(), but bounded by this instance's own VCN range instead
  // of real_size, which continuation instances leave at 0.
  [[nodiscard]] std::optional<ULONGLONG>
      ReadExtentData(ULONGLONG offset, const std::span<BYTE>& buffer) const;

  [[nodiscard]] ULONGLONG GetStartVcn() const noexcept;
  [[nodiscard]] ULONGLONG GetLastVcn() const noexcept;
  [[nodiscard]] ULONGLONG MappedClusters() const noexcept;

  // Splices other's own runs onto this instance's, as the next VCN range.
  void AppendRuns(const AttrNonResident& other);

 public:
  [[nodiscard]] const BYTE* GetData() const noexcept override;
  [[nodiscard]] ULONGLONG GetDataSize() const noexcept override;
  [[nodiscard]] ULONGLONG GetAllocatedSize() const noexcept override;
  [[nodiscard]] std::optional<ULONGLONG>
      ReadData(ULONGLONG offset, const std::span<BYTE>& buffer) const override;
};  // AttrNonResident

}  // namespace Attr

}  // namespace NtfsBrowser
