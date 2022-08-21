#include "attr-file-name.h"
#include "flag/filename.h"
#include "ntfs-common.h"
#include <cassert>

namespace NtfsBrowser
{

AttrFileName::AttrFileName(const AttrHeaderCommon& ahc, const FileRecord& fr)
    : AttrResident(ahc, fr)
{
  NTFS_TRACE("Attribute: File Name\n");

  SetFilename(*reinterpret_cast<const Attr::Filename*>(GetData()));
}

AttrFileName::~AttrFileName() { NTFS_TRACE("AttrFileName deleted\n"); }
#if 0
void AttrFileName::GetFileTime(FILETIME* /*writeTm*/, FILETIME* /*createTm*/,
                               FILETIME* /* accessTm*/) const noexcept
{
  assert(false);
}

Flag::Filename AttrFileName::GetFilePermission() const noexcept
{
  assert(false);
  return Flag::Filename::NONE;
}

bool AttrFileName::IsReadOnly() const noexcept
{
  assert(false);
  return false;
}

bool AttrFileName::IsHidden() const noexcept
{
  assert(false);
  return false;
}

bool AttrFileName::IsSystem() const noexcept
{
  assert(false);
  return false;
}

bool AttrFileName::IsCompressed() const noexcept
{
  assert(false);
  return false;
}

bool AttrFileName::IsEncrypted() const noexcept
{
  assert(false);
  return false;
}

bool AttrFileName::IsSparse() const noexcept
{
  assert(false);
  return false;
}
#endif
}  // namespace NtfsBrowser
