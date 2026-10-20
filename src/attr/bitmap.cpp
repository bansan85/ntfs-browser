#include "attr/bitmap.h"

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>

#include "attr/non-resident.h"
#include "attr/resident.h"

namespace NTFS_BROWSER_NAMESPACE::Attr {

template <class Resident, Cache::Strategy S>
AttrBitmap<Resident, S>::AttrBitmap(const HeaderCommon& ahc,
                                    const Io::FileRecord<S>& file_record)
    : Resident(ahc, file_record) {
  Log::Trace("Attribute: Bitmap ({}Resident)",
             this->IsNonResident() ? "Non" : "");
}

template class AttrBitmap<AttrNonResident<Cache::Strategy::FullCache>,
                          Cache::Strategy::FullCache>;
template class AttrBitmap<AttrNonResident<Cache::Strategy::NoCache>,
                          Cache::Strategy::NoCache>;
template class AttrBitmap<AttrResidentFullCache, Cache::Strategy::FullCache>;
template class AttrBitmap<AttrResidentNoCache, Cache::Strategy::NoCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
