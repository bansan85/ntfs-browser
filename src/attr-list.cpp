#include "attr-list.h"
#include "attr-non-resident.h"
#include "attr-resident.h"
#include "attr/attribute-list.h"
#include "data/run-entry.h"
#include "ntfs-common.h"
#include <ntfs-browser/mask.h>

namespace NtfsBrowser
{

template <typename TYPE_RESIDENT>
AttrList<TYPE_RESIDENT>::AttrList(const AttrHeaderCommon& ahc, FileRecord& fr)
    : TYPE_RESIDENT(ahc, fr)
{
  NTFS_TRACE("Attribute: Attribute List\n");
  if (!fr.file_reference_)
  {
    return;
  }

  ULONGLONG offset = 0;
  ULONGLONG len = 0;
  Attr::AttributeList al_record{};

  while (this->ReadData(offset, &al_record, sizeof(Attr::AttributeList), len) &&
         len == sizeof(Attr::AttributeList))
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
      FileRecord& frnew = file_record_list_.back();

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
      std::vector<std::unique_ptr<AttrBase>>& vec =
          frnew.getAttr(al_record.attr_type);
      for (std::unique_ptr<AttrBase>& veci : vec)
      {
        fr.attr_list_[ATTR_INDEX(al_record.attr_type)].push_back(
            std::move(veci));
      }
      // Throw away frnew.AttrList entries to prevent free twice (fr will delete them)
      vec.clear();
    }

    offset += al_record.record_size;
  }
}

template <typename TYPE_RESIDENT>
AttrList<TYPE_RESIDENT>::~AttrList()
{
  NTFS_TRACE("AttrList deleted\n");
}

template class AttrList<AttrNonResident>;
template class AttrList<AttrResident>;

}  // namespace NtfsBrowser