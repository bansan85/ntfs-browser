#pragma once

#include <vector>
#include <optional>

// OK

#include <ntfs-browser/attr-base.h>

namespace NtfsBrowser
{
namespace Data
{
struct RunEntry;
}  // namespace Data
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
  virtual ~AttrNonResident() = default;

 private:
  const Attr::HeaderNonResident& attr_header_nr_;
  std::vector<Data::RunEntry> data_run_list_;
  bool data_run_ok_;
  static bool PickData(const BYTE*& dataRun, ULONGLONG& length,
                       LONGLONG& LCNOffset) noexcept;
  bool ParseDataRun();
  bool ReadClusters(void* buf, DWORD clusters,
                    std::optional<ULONGLONG> start_lcn, ULONGLONG offset) const;
  bool ReadVirtualClusters(ULONGLONG vcn, DWORD clusters, void* bufv,
                           DWORD bufLen, DWORD& actural) const;

 protected:
  bool IsDataRunOK() const noexcept override;

 public:
  [[nodiscard]] ULONGLONG GetDataSize() const noexcept override;
  [[nodiscard]] bool ReadData(ULONGLONG offset, gsl::not_null<void*> bufv,
                              DWORD bufLen, DWORD& actural) const override;
};  // AttrNonResident
}  // namespace NtfsBrowser