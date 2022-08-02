#include "attr-vol-name.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

AttrVolName::AttrVolName(const AttrHeaderCommon* ahc, const FileRecord* fr)
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

AttrVolName::~AttrVolName()
{
  NTFS_TRACE("AttrVolName deleted\n");

  delete VolNameU;
  delete VolNameA;
}

// Get NTFS Volume Unicode Name
int AttrVolName::GetName(wchar_t* buf, DWORD len) const
{
  if (len < NameLength) return -1 * NameLength;  // buffer too small

  wcsncpy(buf, VolNameU, NameLength + 1);
  return NameLength;
}

// ANSI Name
int AttrVolName::GetName(char* buf, DWORD len) const
{
  if (len < NameLength) return -1 * NameLength;  // buffer too small

  strncpy(buf, VolNameA, NameLength + 1);
  return NameLength;
}
}  // namespace NtfsBrowser
