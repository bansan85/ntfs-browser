#pragma once

#include <optional>
#include <vector>

// OK

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
  virtual ~AttrNonResident() = default;

 private:
  const Attr::HeaderNonResident& attr_header_nr_;
  std::vector<Data::RunEntry> data_run_list_;
  bool data_run_ok_;
  [[nodiscard]] static bool PickData(const BYTE*& dataRun, ULONGLONG& length,
                                     LONGLONG& LCNOffset) noexcept;
  [[nodiscard]] bool ParseDataRun();
  [[nodiscard]] bool ReadClusters(void* buf, DWORD clusters,
                                  std::optional<ULONGLONG> start_lcn,
                                  ULONGLONG offset) const;
  [[nodiscard]] bool ReadVirtualClusters(ULONGLONG vcn, DWORD clusters,
                                         void* bufv, DWORD bufLen,
                                         DWORD& actural) const;

 protected:
  [[nodiscard]] bool IsDataRunOK() const noexcept override;

 public:
  [[nodiscard]] ULONGLONG GetDataSize() const noexcept override;
  [[nodiscard]] bool ReadData(ULONGLONG offset, gsl::not_null<void*> bufv,
                              DWORD bufLen, DWORD& actural) const override;
};  // AttrNonResident
}  // namespace NtfsBrowser