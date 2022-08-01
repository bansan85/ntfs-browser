#pragma once

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/ntfs-common.h>
#include <ntfs-browser/data/attr-attribute-list.h>

namespace NtfsBrowser
{

////////////////////////////////////////////
// Attribute: Attribute List
////////////////////////////////////////////
template <typename TYPE_RESIDENT>
class AttrList : public TYPE_RESIDENT
{
 public:
  AttrList(const AttrHeaderCommon* ahc, const FileRecord* fr)
      : TYPE_RESIDENT(ahc, fr)
  {
    NTFS_TRACE("Attribute: Attribute List\n");
    if (fr->FileReference == (ULONGLONG)-1) return;

    ULONGLONG offset = 0;
    DWORD len;
    AttrAttributeList alRecord;

    while (ReadData(offset, &alRecord, sizeof(AttrAttributeList), &len) &&
           len == sizeof(AttrAttributeList))
    {
      if (ATTR_INDEX(alRecord.AttrType) > kAttrNums)
      {
        NTFS_TRACE("Attribute List parse error1\n");
        break;
      }

      NTFS_TRACE1("Attribute List: 0x%04x\n", alRecord.AttrType);

      ULONGLONG recordRef = alRecord.BaseRef & 0x0000FFFFFFFFFFFFUL;
      if (recordRef != fr->FileReference)  // Skip contained attributes
      {
        DWORD am = ATTR_MASK(alRecord.AttrType);
        if (am & fr->AttrMask)  // Skip unwanted attributes
        {
          FileRecord* frnew = new FileRecord(fr->Volume);
          FileRecordList.push_back(*frnew);

          frnew->AttrMask = am;
          if (!frnew->ParseFileRecord(recordRef))
          {
            NTFS_TRACE("Attribute List parse error2\n");
            break;
          }
          frnew->ParseAttrs();

          // Insert new found AttrList to fr->AttrList
          const std::vector<AttrBase*>* vec = frnew->getAttr(alRecord.AttrType);
          if (vec != NULL)
          {
            for (AttrBase* ab : *vec)
            {
              auto vec2 = fr->attr_list_[ATTR_INDEX(alRecord.AttrType)];
              vec2.push_back(ab);
            }
          }

          // Throw away frnew->AttrList entries to prevent free twice (fr will delete them)
          frnew->attr_list_[ATTR_INDEX(alRecord.AttrType)].clear();
        }
      }

      offset += alRecord.RecordSize;
    }
  }

  virtual ~AttrList() { NTFS_TRACE("AttrList deleted\n"); }

 private:
  std::vector<FileRecord> FileRecordList;
};  // AttrList

}  // namespace NtfsBrowser