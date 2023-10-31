#include "attr-list.h"
#include "attr-non-resident.h"
#include "attr-resident.h"
#include "attr/attribute-list.h"
#include "data/run-entry.h"
#include "ntfs-common.h"
#include <ntfs-browser/mask.h>

namespace NtfsBrowser
{

template <typename TYPE_RESIDENT, Strategy S>
AttrList<TYPE_RESIDENT, S>::AttrList(const AttrHeaderCommon& ahc,
                                     FileRecord<S>& fr)
    : TYPE_RESIDENT(ahc, fr)
{
  NTFS_TRACE("Attribute: Attribute List\n");
  if (!fr.file_reference_)
  {
    return;
  }

  ULONGLONG offset = 0;
  std::optional<ULONGLONG> len = 0;
  Attr::AttributeList al_record{};

  while ((len = this->ReadData(offset, {reinterpret_cast<BYTE*>(&al_record),
                                        sizeof(Attr::AttributeList)})) &&
         *len == sizeof(Attr::AttributeList))
  {
    if (ATTR_INDEX(al_record.attr_type) > kAttrNums)
    {
      NTFS_TRACE("Attribute List parse error1\n");
      break;
    }

    NTFS_TRACE1("Attribute List: 0x%04x\n", al_record.attr_type);

    const ULONGLONG record_ref = al_record.base_ref.segment_number;
    const Mask am = ATTR_MASK(al_record.attr_type);
    // Skip contained attributes
    // Skip unwanted attributes
    if (record_ref != *fr.file_reference_ &&
        static_cast<bool>(am & fr.attr_mask_))
    {
      file_record_list_.emplace_back(fr.volume_);
      FileRecord<S>& frnew = file_record_list_.back();

      frnew.attr_mask_ = am;
      if (!frnew.ParseFileRecord(record_ref))
      {
        NTFS_TRACE("Attribute List parse error2\n");
        break;
      }
      if (!frnew.ParseAttrs())
      {
        NTFS_TRACE("Attribute List parse error3\n");
        break;
      }

      // Insert new found AttrList to fr.AttrList
      std::vector<std::unique_ptr<AttrBase<S>>>& vec =
          frnew.getAttr(al_record.attr_type);
      for (std::unique_ptr<AttrBase<S>>& veci : vec)
      {
        fr.attr_list_[ATTR_INDEX(al_record.attr_type)].push_back(
            std::move(veci));
      }
      vec.clear();
    }

    if (al_record.record_size == 0)
    {
      break;
    }
    offset += al_record.record_size;
  }
}

template <typename TYPE_RESIDENT, Strategy S>
AttrList<TYPE_RESIDENT, S>::~AttrList()
{
  NTFS_TRACE("AttrList deleted\n");
}

template class AttrList<AttrNonResident<Strategy::FULL_CACHE>,
                        Strategy::FULL_CACHE>;
template class AttrList<AttrNonResident<Strategy::NO_CACHE>,
                        Strategy::NO_CACHE>;
template class AttrList<AttrResidentFullCache, Strategy::FULL_CACHE>;
template class AttrList<AttrResidentNoCache, Strategy::NO_CACHE>;

}  // namespace NtfsBrowser