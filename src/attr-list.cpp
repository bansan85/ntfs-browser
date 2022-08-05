#include "attr-list.h"
#include "attr-non-resident.h"
#include "attr-resident.h"
#include "ntfs-common.h"
#include "attr/attribute-list.h"
#include <ntfs-browser/mask.h>

namespace NtfsBrowser
{

////////////////////////////////////////////
// Attribute: Attribute List
////////////////////////////////////////////
template <typename TYPE_RESIDENT>
AttrList<TYPE_RESIDENT>::AttrList(const AttrHeaderCommon& ahc,
                                  const FileRecord& fr)
    : TYPE_RESIDENT(ahc, fr)
{
  NTFS_TRACE("Attribute: Attribute List\n");
  if (fr.file_reference_ == (ULONGLONG)-1) return;

  ULONGLONG offset = 0;
  DWORD len;
  Attr::AttributeList alRecord;

  while (this->ReadData(offset, &alRecord, sizeof(Attr::AttributeList), len) &&
         len == sizeof(Attr::AttributeList))
  {
    if (ATTR_INDEX(alRecord.AttrType) > kAttrNums)
    {
      NTFS_TRACE("Attribute List parse error1\n");
      break;
    }

    NTFS_TRACE1("Attribute List: 0x%04x\n", alRecord.AttrType);

    ULONGLONG recordRef = alRecord.BaseRef & 0x0000FFFFFFFFFFFFUL;
    if (recordRef != fr.file_reference_)  // Skip contained attributes
    {
      Mask am = ATTR_MASK(alRecord.AttrType);
      if (static_cast<BOOL>(am & fr.attr_mask_))  // Skip unwanted attributes
      {
        FileRecordList.emplace_back(fr.volume_);
        FileRecord& frnew = FileRecordList.back();

        frnew.attr_mask_ = am;
        if (!frnew.ParseFileRecord(recordRef))
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
        const std::vector<AttrBase*>* vec = frnew.getAttr(alRecord.AttrType);
        if (vec != nullptr)
        {
          for (AttrBase* ab : *vec)
          {
            auto vec2 = fr.attr_list_[ATTR_INDEX(alRecord.AttrType)];
            vec2.push_back(ab);
          }
        }

        // Throw away frnew.AttrList entries to prevent free twice (fr will delete them)
        frnew.attr_list_[ATTR_INDEX(alRecord.AttrType)].clear();
      }
    }

    offset += alRecord.RecordSize;
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