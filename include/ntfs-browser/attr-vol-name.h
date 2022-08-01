#pragma once

#include <ntfs-browser/attr-resident.h>
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>

namespace NtfsBrowser
{

///////////////////////////
// Attribute: Volume Name
///////////////////////////
class AttrVolName : public AttrResident
{
 public:
  AttrVolName(const AttrHeaderCommon* ahc, const FileRecord* fr)
      : AttrResident(ahc, fr)
  {
    NTFS_TRACE("Attribute: Volume Name\n");

    NameLength = AttrBodySize >> 1;
    VolNameU = new wchar_t[NameLength + 1];
    VolNameA = new char[NameLength + 1];

    memcpy(VolNameU, AttrBody, AttrBodySize);
    VolNameU[NameLength] = wchar_t('\0');

    int len = WideCharToMultiByte(CP_ACP, 0, VolNameU, NameLength, VolNameA,
                                  NameLength, NULL, NULL);
    VolNameA[NameLength] = '\0';
  }

  virtual ~AttrVolName()
  {
    NTFS_TRACE("AttrVolName deleted\n");

    delete VolNameU;
    delete VolNameA;
  }

 private:
  wchar_t* VolNameU;
  char* VolNameA;
  DWORD NameLength;

 public:
  // Get NTFS Volume Unicode Name
  int GetName(wchar_t* buf, DWORD len) const
  {
    if (len < NameLength) return -1 * NameLength;  // buffer too small

    wcsncpy(buf, VolNameU, NameLength + 1);
    return NameLength;
  }

  // ANSI Name
  int GetName(char* buf, DWORD len) const
  {
    if (len < NameLength) return -1 * NameLength;  // buffer too small

    strncpy(buf, VolNameA, NameLength + 1);
    return NameLength;
  }
};  // AttrVolInfo

}  // namespace NtfsBrowser