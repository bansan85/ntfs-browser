#include "attr-vol-name.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

template <typename RESIDENT>
AttrVolName<RESIDENT>::AttrVolName(const AttrHeaderCommon& ahc,
                                   const FileRecord& fr)
    : RESIDENT(ahc, fr)
{
  NTFS_TRACE("Attribute: Volume Name\n");

  name_.resize((this->GetDataSize() / 2) + 1, '\0');
  memcpy(name_.data(), this->GetData(), this->GetDataSize());
}

// Get NTFS Volume Unicode Name
template <typename RESIDENT>
std::wstring_view AttrVolName<RESIDENT>::GetName() const noexcept
{
  return name_;
}

template class AttrVolName<AttrResidentHeavy>;
template class AttrVolName<AttrResidentLight>;

}  // namespace NtfsBrowser
