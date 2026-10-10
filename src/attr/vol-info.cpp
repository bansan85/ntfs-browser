#include "attr/vol-info.h"

#include <stdexcept>

#include <ntfs-browser/cache/strategy.h>

#include "attr/resident.h"
#include "data/volume-information.h"
#include "log/ntfs-common.h"
#include "ntfs-browser/win-types.h"

namespace NtfsBrowser::Attr {

namespace {

// Checks the body size before a reference is bound to it: an empty body may
// have a null data pointer, which a reference MUST NOT be bound to.
template <typename Resident>
const Data::VolumeInformation& CheckedVolInfo(const Resident& attr) {
  if (attr.GetDataSize() < sizeof(Data::VolumeInformation)) {
    throw std::runtime_error(
        "Volume Information attribute smaller than expected.\n");
  }

  return *reinterpret_cast<const Data::VolumeInformation*>(attr.GetData());
}

}  // namespace

template <typename Resident, Cache::Strategy S>
AttrVolInfo<Resident, S>::AttrVolInfo(const HeaderCommon& ahc,
                                      const Io::FileRecord<S>& file_record)
    : Resident(ahc, file_record), vol_info_(CheckedVolInfo<Resident>(*this)) {
  Log::Trace("Attribute: Volume Information");
}

template <typename Resident, Cache::Strategy S>
AttrVolInfo<Resident, S>::~AttrVolInfo() {
  Log::Trace("AttrVolInfo deleted");
}

template <typename Resident, Cache::Strategy S>
std::pair<BYTE, BYTE> AttrVolInfo<Resident, S>::GetVersion() const noexcept {
  return {vol_info_.major_version, vol_info_.minor_version};
}

template class AttrVolInfo<AttrResidentFullCache, Cache::Strategy::FullCache>;
template class AttrVolInfo<AttrResidentNoCache, Cache::Strategy::NoCache>;

}  // namespace NtfsBrowser::Attr
