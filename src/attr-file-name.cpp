#include "attr-file-name.h"
#include "attr/filename.h"
#include "flag/filename.h"
#include "ntfs-common.h"
#include <cassert>
#include <cstddef>
#include <stdexcept>

namespace NtfsBrowser
{

template <typename RESIDENT, Strategy S>
AttrFileName<RESIDENT, S>::AttrFileName(const AttrHeaderCommon& ahc,
                                        const FileRecord<S>& fr)
    : RESIDENT(ahc, fr)
{
  NTFS_TRACE("Attribute: File Name\n");

  if (this->GetDataSize() < offsetof(Attr::Filename, name))
  {
    throw std::runtime_error("File Name attribute smaller than expected.\n");
  }

  const auto& fn = *reinterpret_cast<const Attr::Filename*>(this->GetData());
  if (this->GetDataSize() <
      offsetof(Attr::Filename, name) +
          (static_cast<ULONGLONG>(fn.name_length) * sizeof(WORD)))
  {
    throw std::runtime_error(
        "File Name attribute name exceeds attribute bounds.\n");
  }

  SetFilename(fn);
}

template <typename RESIDENT, Strategy S>
AttrFileName<RESIDENT, S>::~AttrFileName()
{
  NTFS_TRACE("AttrFileName deleted\n");
}
#if 0
template <typename RESIDENT>
void AttrFileName<RESIDENT>::GetFileTime(FILETIME* /*writeTm*/, FILETIME* /*createTm*/,
                               FILETIME* /* accessTm*/) const noexcept
{
  assert(false);
}

template <typename RESIDENT>
Flag::Filename AttrFileName<RESIDENT>::GetFilePermission() const noexcept
{
  assert(false);
  return Flag::Filename::NONE;
}

template <typename RESIDENT>
bool AttrFileName<RESIDENT>::IsReadOnly() const noexcept
{
  assert(false);
  return false;
}

template <typename RESIDENT>
bool AttrFileName<RESIDENT>::IsHidden() const noexcept
{
  assert(false);
  return false;
}

template <typename RESIDENT>
bool AttrFileName<RESIDENT>::IsSystem() const noexcept
{
  assert(false);
  return false;
}

template <typename RESIDENT>
bool AttrFileName<RESIDENT>::IsCompressed() const noexcept
{
  assert(false);
  return false;
}

template <typename RESIDENT>
bool AttrFileName<RESIDENT>::IsEncrypted() const noexcept
{
  assert(false);
  return false;
}

template <typename RESIDENT>
bool AttrFileName<RESIDENT>::IsSparse() const noexcept
{
  assert(false);
  return false;
}
#endif

template class AttrFileName<AttrResidentFullCache, Strategy::FULL_CACHE>;
template class AttrFileName<AttrResidentNoCache, Strategy::NO_CACHE>;

}  // namespace NtfsBrowser
