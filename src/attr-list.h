#pragma once

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/data/attr-header-common.h>
#include "data/file-record-header.h"  //for complete type FileRecord

namespace NtfsBrowser
{
template <typename TYPE_RESIDENT>
class AttrList : public TYPE_RESIDENT
{
 public:
  AttrList(const AttrHeaderCommon& ahc, const FileRecord& fr);

  virtual ~AttrList();

 private:
  std::vector<FileRecord> FileRecordList;
};  // AttrList

}  // namespace NtfsBrowser