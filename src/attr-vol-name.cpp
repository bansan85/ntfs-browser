#include "attr-vol-name.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

template <typename RESIDENT, Strategy S>
AttrVolName<RESIDENT, S>::AttrVolName(const AttrHeaderCommon& ahc,
                                      const FileRecord<S>& fr)
    : RESIDENT(ahc, fr)
{
  NTFS_TRACE("Attribute: Volume Name\n");

  name_.resize((this->GetDataSize() / 2) + 1, '\0');
  memcpy(name_.data(), this->GetData(), this->GetDataSize());
}

// Get NTFS Volume Unicode Name
template <typename RESIDENT, Strategy S>
std::wstring_view AttrVolName<RESIDENT, S>::GetName() const noexcept
{
  return name_;
}

template class AttrVolName<AttrResidentFullCache, Strategy::FULL_CACHE>;
template class AttrVolName<AttrResidentNoCache, Strategy::NO_CACHE>;

}  // namespace NtfsBrowser
