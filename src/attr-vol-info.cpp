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

namespace
{
// Checks the body size before a reference is bound to it: an empty body may
// have a null data pointer, which a reference MUST NOT be bound to.
template <typename RESIDENT>
const Attr::VolumeInformation& CheckedVolInfo(const RESIDENT& attr)
{
  if (attr.GetDataSize() < sizeof(Attr::VolumeInformation))
  {
    throw std::runtime_error(
        "Volume Information attribute smaller than expected.\n");
  }

  return *reinterpret_cast<const Attr::VolumeInformation*>(attr.GetData());
}
}  // namespace

template <typename RESIDENT, Strategy S>
AttrVolInfo<RESIDENT, S>::AttrVolInfo(const AttrHeaderCommon& ahc,
                                      const FileRecord<S>& fr)
    : RESIDENT(ahc, fr), vol_info_(CheckedVolInfo<RESIDENT>(*this))
{
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
