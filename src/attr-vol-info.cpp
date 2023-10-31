#include "attr-vol-info.h"
#include "attr/volume-information.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

template <typename RESIDENT, Strategy S>
AttrVolInfo<RESIDENT, S>::AttrVolInfo(const AttrHeaderCommon& ahc,
                                      const FileRecord<S>& fr)
    : RESIDENT(ahc, fr),
      vol_info_(
          *reinterpret_cast<const Attr::VolumeInformation*>(this->GetData()))
{
  NTFS_TRACE("Attribute: Volume Information\n");
}

template <typename RESIDENT, Strategy S>
AttrVolInfo<RESIDENT, S>::~AttrVolInfo()
{
  NTFS_TRACE("AttrVolInfo deleted\n");
}

template <typename RESIDENT, Strategy S>
std::pair<BYTE, BYTE> AttrVolInfo<RESIDENT, S>::GetVersion() const noexcept
{
  return {vol_info_.major_version, vol_info_.minor_version};
}

template class AttrVolInfo<AttrResidentFullCache, Strategy::FULL_CACHE>;
template class AttrVolInfo<AttrResidentNoCache, Strategy::NO_CACHE>;

}  // namespace NtfsBrowser
