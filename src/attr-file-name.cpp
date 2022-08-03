#include "attr-file-name.h"
#include "ntfs-common.h"
#include <cassert>

namespace NtfsBrowser
{

AttrFileName::AttrFileName(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : AttrResident(ahc, fr)
{
  NTFS_TRACE("Attribute: File Name\n");

  SetFilename((Attr::Filename*)attr_body_);
}

AttrFileName::~AttrFileName() { NTFS_TRACE("AttrFileName deleted\n"); }

void AttrFileName::GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                               FILETIME* accessTm) const
{
  assert(false);
}
DWORD AttrFileName::GetFilePermission()
{
  assert(false);
  return 0;
}
BOOL AttrFileName::IsReadOnly() const
{
  assert(false);
  return false;
}
BOOL AttrFileName::IsHidden() const
{
  assert(false);
  return false;
}
BOOL AttrFileName::IsSystem() const
{
  assert(false);
  return false;
}
BOOL AttrFileName::IsCompressed() const
{
  assert(false);
  return false;
}
BOOL AttrFileName::IsEncrypted() const
{
  assert(false);
  return false;
}
BOOL AttrFileName::IsSparse() const
{
  assert(false);
  return false;
}

}  // namespace NtfsBrowser
