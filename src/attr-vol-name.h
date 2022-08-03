#pragma once

#include "attr-resident.h"
#include <string>
#include <string_view>

// OK

namespace NtfsBrowser
{
struct AttrHeaderCommon;
class FileRecord;

///////////////////////////
// Attribute: Volume Name
///////////////////////////
class AttrVolName : public AttrResident
{
 public:
  AttrVolName(const AttrHeaderCommon& ahc, const FileRecord& fr);

  ~AttrVolName() override = default;

 private:
  std::wstring name_;

 public:
  // Get NTFS Volume Unicode Name
  [[nodiscard]] std::wstring_view GetName() const;
};  // AttrVolInfo

}  // namespace NtfsBrowser