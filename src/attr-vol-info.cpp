#include "attr-vol-info.h"
#include "attr/volume-information.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

template <typename RESIDENT>
AttrVolInfo<RESIDENT>::AttrVolInfo(const AttrHeaderCommon& ahc,
                                   const FileRecord& fr)
    : RESIDENT(ahc, fr),
      vol_info_(
          *reinterpret_cast<const Attr::VolumeInformation*>(this->GetData()))
{
  NTFS_TRACE("Attribute: Volume Information\n");
}

template <typename RESIDENT>
AttrVolInfo<RESIDENT>::~AttrVolInfo()
{
  NTFS_TRACE("AttrVolInfo deleted\n");
}

template <typename RESIDENT>
std::pair<BYTE, BYTE> AttrVolInfo<RESIDENT>::GetVersion() const noexcept
{
  return {vol_info_.major_version, vol_info_.minor_version};
}

template class AttrVolInfo<AttrResidentHeavy>;
template class AttrVolInfo<AttrResidentLight>;

}  // namespace NtfsBrowser
