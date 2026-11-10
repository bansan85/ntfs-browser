#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/types.h"
#include "attr/base.h"
#include "attr/compression-units.h"
#include "attr/non-resident-layout.h"
#include "attr/run-list.h"
#include "cache/view.h"
#include "internal-export.h"
#include "win-types.h"

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

  // Set only on a compressed stream, which ReadData() then reads through it.
  std::optional<CompressionUnits<S>> compression_;

  // Set only on an encrypted stream. Shared with the record's other encrypted
  // streams, which hold the same key.
  std::shared_ptr<const Efs::Context> efs_context_;

  // Answers the layout questions over runs_ and the header. Set at the end of
  // the constructor body, once compression_ is known.
  std::optional<NonResidentLayout<S>> layout_;

  void ParseDataRun();
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

  // The runs of real clusters a ReadExtentData() of length bytes at offset
  // reads, in order, the partial clusters at both ends included. A sparse part
  // has none, and neither has a compressed stream, which reads whole units.
  // Reads nothing.
  [[nodiscard]] std::vector<Alloc::Extent>
      ExtentsOfRange(ULONGLONG offset, ULONGLONG length) const;
};  // AttrNonResident

}  // namespace Attr

}  // namespace NTFS_BROWSER_NAMESPACE
