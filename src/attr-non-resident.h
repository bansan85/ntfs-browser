#pragma once

#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/attr-base.h>

#include "data/run-entry.h"

namespace NtfsBrowser
{
namespace Attr
{
struct HeaderNonResident;
}  // namespace Attr
////////////////////////////////
// NonResident Attributes
////////////////////////////////
class AttrNonResident : public AttrBase
{
 public:
  AttrNonResident(const AttrHeaderCommon& ahc, const FileRecord& fr);
  AttrNonResident(AttrNonResident&& other) noexcept = delete;
  AttrNonResident(AttrNonResident const& other) = delete;
  AttrNonResident& operator=(AttrNonResident&& other) noexcept = delete;
  AttrNonResident& operator=(AttrNonResident const& other) = delete;
  ~AttrNonResident() override = default;

 private:
  const Attr::HeaderNonResident& attr_header_nr_;
  std::vector<Data::RunEntry> data_run_list_;
  bool data_run_ok_;
  [[nodiscard]] static bool PickData(const BYTE*& dataRun, ULONGLONG& length,
                                     LONGLONG& LCNOffset) noexcept;
  [[nodiscard]] bool ParseDataRun();
  [[nodiscard]] std::optional<std::span<const BYTE>>
      ReadClusters(ULONGLONG clusters, ULONGLONG start_lcn,
                   ULONGLONG offset) const;
  [[nodiscard]] std::optional<ULONGLONG>
      ReadVirtualClusters(ULONGLONG vcn, ULONGLONG clusters,
                          std::span<BYTE> buffer) const;

 protected:
  [[nodiscard]] bool IsDataRunOK() const noexcept override;

 public:
  [[nodiscard]] const BYTE* GetData() const noexcept override;
  [[nodiscard]] ULONGLONG GetDataSize() const noexcept override;
  [[nodiscard]] std::optional<ULONGLONG>
      ReadData(ULONGLONG offset, std::span<BYTE> buffer) const override;
};  // AttrNonResident
}  // namespace NtfsBrowser