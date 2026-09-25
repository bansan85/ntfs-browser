#pragma once

#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/strategy.h>

#include "data/run-entry.h"

namespace NtfsBrowser
{
namespace Attr
{
struct HeaderNonResident;
}  // namespace Attr
namespace Efs
{
class Context;
}  // namespace Efs
////////////////////////////////
// NonResident Attributes
////////////////////////////////
template <Strategy S>
class AttrNonResident : public AttrBase<S>
{
 public:
  AttrNonResident(const AttrHeaderCommon& ahc, const FileRecord<S>& fr);
  AttrNonResident(AttrNonResident&& other) noexcept = delete;
  AttrNonResident(AttrNonResident const& other) = delete;
  AttrNonResident& operator=(AttrNonResident&& other) noexcept = delete;
  AttrNonResident& operator=(AttrNonResident const& other) = delete;
  ~AttrNonResident() override = default;

 private:
  const Attr::HeaderNonResident& attr_header_nr_;
  std::vector<Data::RunEntry> data_run_list_;

  // Clusters per compression unit (2^comp_unit_size); 0 means uncompressed.
  ULONGLONG comp_unit_clusters_{0};

  // Decompressed compression units, keyed by unit index; lifetime follows
  // Strategy (FULL_CACHE keeps them, NO_CACHE clears per ReadData()).
  mutable std::unordered_map<ULONGLONG, std::vector<BYTE>> comp_unit_cache_;

  // Set only on an encrypted stream. Shared with the record's other encrypted
  // streams, which hold the same key.
  std::shared_ptr<const Efs::Context> efs_context_;

  [[nodiscard]] static bool PickData(const BYTE*& dataRun, const BYTE* end,
                                     ULONGLONG& length,
                                     LONGLONG& LCNOffset) noexcept;
  void ParseDataRun();
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
  [[nodiscard]] ULONGLONG UnitClusters(ULONGLONG unitFirstVcn) const noexcept;
  [[nodiscard]] std::optional<ULONGLONG>
      LeadingRealClusters(ULONGLONG unitFirstVcn,
                          ULONGLONG unitClusters) const noexcept;
  [[nodiscard]] const std::vector<BYTE>*
      GetCompressionUnit(ULONGLONG unitIndex) const;
  [[nodiscard]] std::optional<ULONGLONG>
      ReadVirtualClustersCompressed(ULONGLONG vcn, ULONGLONG clusters,
                                    std::span<BYTE> buffer) const;

 public:
  // Makes ReadData() decrypt this stream with the given context.
  void SetEfsContext(std::shared_ptr<const Efs::Context> context) noexcept;

  [[nodiscard]] const BYTE* GetData() const noexcept override;
  [[nodiscard]] ULONGLONG GetDataSize() const noexcept override;
  [[nodiscard]] std::optional<ULONGLONG>
      ReadData(ULONGLONG offset, const std::span<BYTE>& buffer) const override;

  // Like ReadData(), but bounded by this instance's own VCN range instead
  // of real_size, which continuation instances leave at 0.
  [[nodiscard]] std::optional<ULONGLONG>
      ReadExtentData(ULONGLONG offset, const std::span<BYTE>& buffer) const;

  [[nodiscard]] ULONGLONG GetStartVcn() const noexcept;
  [[nodiscard]] ULONGLONG GetLastVcn() const noexcept;
};  // AttrNonResident
}  // namespace NtfsBrowser