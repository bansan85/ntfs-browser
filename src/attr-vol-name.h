#pragma once

#include "attr-resident.h"

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

  virtual ~AttrVolName();

 private:
  wchar_t* VolNameU;
  char* VolNameA;
  DWORD NameLength;

 public:
  // Get NTFS Volume Unicode Name
  int GetName(wchar_t* buf, DWORD len) const;
  // ANSI Name
  int GetName(char* buf, DWORD len) const;
};  // AttrVolInfo

}  // namespace NtfsBrowser