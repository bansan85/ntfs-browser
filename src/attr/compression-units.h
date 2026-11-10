#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Data {

struct HeaderNonResident;

}  // namespace Data

namespace Attr {

class RunList;

// The compressed reading of one non-resident stream. The stream is cut in
// compression units of a fixed count of clusters. A unit is stored as it is,
// LZNT1 compressed, or sparse. This class decodes whole units and serves
// virtual clusters out of them.
template <Cache::Strategy S>
class CompressionUnits {
 public:
  // Reads clusters of the stream as stored, with no decoding, into buffer.
  // Gives the bytes read, or nothing on a failure.
  using RawReader = std::function<std::optional<ULONGLONG>(
      ULONGLONG vcn, ULONGLONG clusters, std::span<BYTE> buffer)>;

  // What the units are cut from. header and runs MUST outlive the units.
  struct Source {
    const Data::HeaderNonResident& header;
    const RunList& runs;
    ULONGLONG cluster_size;
    RawReader raw_reader;
  };

  // Throws when the header declares an unusable unit size, a start VCN off a
  // unit boundary, or when decompression is not compiled in.
  explicit CompressionUnits(Source source);

  // Clusters per full compression unit.
  [[nodiscard]] ULONGLONG ClustersPerUnit() const noexcept;

  // Starts one read of the stream. NoCache keeps no unit from a read before.
  void BeginRead() const;

  // Compressed counterpart of a raw cluster read: serves the virtual clusters
  // out of whole units. buffer MUST hold exactly clusters clusters. Gives the
  // bytes copied, which is short when a unit cannot be made.
  [[nodiscard]] std::optional<ULONGLONG> Read(ULONGLONG vcn, ULONGLONG clusters,
                                              std::span<BYTE> buffer) const;

 private:
  const Data::HeaderNonResident& header_;
  const RunList& runs_;
  ULONGLONG cluster_size_;
  RawReader raw_reader_;

  // Clusters per compression unit (2^comp_unit_size).
  ULONGLONG comp_unit_clusters_{0};

  // Decoded units, keyed by unit index. FullCache keeps them. NoCache drops
  // them at the start of each read.
  mutable std::unordered_map<ULONGLONG, std::vector<BYTE>> units_;

  [[nodiscard]] ULONGLONG UnitClusters(ULONGLONG unit_first_vcn) const noexcept;
  [[nodiscard]] bool DecompressUnit(ULONGLONG unit_index,
                                    ULONGLONG unit_first_vcn,
                                    ULONGLONG real_clusters,
                                    std::vector<BYTE>& unit) const;
  [[nodiscard]] bool FillCompressionUnit(ULONGLONG unit_index,
                                         ULONGLONG unit_first_vcn,
                                         ULONGLONG unit_clusters,
                                         ULONGLONG real_clusters,
                                         std::vector<BYTE>& unit) const;
  [[nodiscard]] std::optional<std::vector<BYTE>>
      BuildUnit(ULONGLONG unit_index) const;
  [[nodiscard]] const std::vector<BYTE>*
      GetCompressionUnit(ULONGLONG unit_index) const;
};  // CompressionUnits

}  // namespace Attr

}  // namespace NTFS_BROWSER_NAMESPACE
