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
template <typename Resident>
const Attr::VolumeInformation& CheckedVolInfo(const Resident& attr)
{
  if (attr.GetDataSize() < sizeof(Attr::VolumeInformation))
  {
    throw std::runtime_error(
        "Volume Information attribute smaller than expected.\n");
  }

  return *reinterpret_cast<const Attr::VolumeInformation*>(attr.GetData());
}
}  // namespace

template <typename Resident, Strategy S>
AttrVolInfo<Resident, S>::AttrVolInfo(const AttrHeaderCommon& ahc,
                                      const FileRecord<S>& file_record)
    : Resident(ahc, file_record), vol_info_(CheckedVolInfo<Resident>(*this))
{
  LogTrace("Attribute: Volume Information");
}

template <typename Resident, Strategy S>
AttrVolInfo<Resident, S>::~AttrVolInfo()
{
  LogTrace("AttrVolInfo deleted");
}

template <typename Resident, Strategy S>
std::pair<BYTE, BYTE> AttrVolInfo<Resident, S>::GetVersion() const noexcept
{
  return {vol_info_.major_version, vol_info_.minor_version};
}

template class AttrVolInfo<AttrResidentFullCache, Strategy::FullCache>;
template class AttrVolInfo<AttrResidentNoCache, Strategy::NoCache>;

}  // namespace NtfsBrowser
