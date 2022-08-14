#pragma once

#include <optional>
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
  [[nodiscard]] bool ReadClusters(void* buf, ULONGLONG clusters,
                                  std::optional<ULONGLONG> start_lcn,
                                  ULONGLONG offset) const;
  [[nodiscard]] bool ReadVirtualClusters(ULONGLONG vcn, ULONGLONG clusters,
                                         void* bufv, ULONGLONG bufLen,
                                         ULONGLONG& actural) const;

 protected:
  [[nodiscard]] bool IsDataRunOK() const noexcept override;

 public:
  [[nodiscard]] ULONGLONG GetDataSize() const noexcept override;
  [[nodiscard]] bool ReadData(ULONGLONG offset, gsl::not_null<void*> bufv,
                              ULONGLONG bufLen,
                              ULONGLONG& actural) const override;
};  // AttrNonResident
}  // namespace NtfsBrowser