#include "attr-vol-info.h"

#include <stdexcept>

#include <ntfs-browser/strategy.h>

#include "attr-resident.h"
#include "attr/volume-information.h"
#include "ntfs-browser/win-types.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{
struct AttrHeaderCommon;
template <Strategy S>
class FileRecord;

template <typename RESIDENT, Strategy S>
AttrVolInfo<RESIDENT, S>::AttrVolInfo(const AttrHeaderCommon& ahc,
                                      const FileRecord<S>& fr)
    : RESIDENT(ahc, fr),
      vol_info_(
          *reinterpret_cast<const Attr::VolumeInformation*>(this->GetData()))
{
  if (this->GetDataSize() < sizeof(Attr::VolumeInformation))
  {
    throw std::runtime_error(
        "Volume Information attribute smaller than expected.\n");
  }

  LogTrace("Attribute: Volume Information");
}

template <typename RESIDENT, Strategy S>
AttrVolInfo<RESIDENT, S>::~AttrVolInfo()
{
  LogTrace("AttrVolInfo deleted");
}

template <typename RESIDENT, Strategy S>
std::pair<BYTE, BYTE> AttrVolInfo<RESIDENT, S>::GetVersion() const noexcept
{
  return {vol_info_.major_version, vol_info_.minor_version};
}

template class AttrVolInfo<AttrResidentFullCache, Strategy::FULL_CACHE>;
template class AttrVolInfo<AttrResidentNoCache, Strategy::NO_CACHE>;

}  // namespace NtfsBrowser
