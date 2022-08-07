#pragma once

#include <ntfs-browser/file-record.h>
#include <ntfs-browser/data/attr-header-common.h>
#include "data/file-record-header.h"  //for complete type FileRecord

// OK

namespace NtfsBrowser
{
template <typename TYPE_RESIDENT>
class AttrList : public TYPE_RESIDENT
{
 public:
  AttrList(const AttrHeaderCommon& ahc, FileRecord& fr);
  AttrList(AttrList&& other) noexcept = delete;
  AttrList(AttrList const& other) = delete;
  AttrList& operator=(AttrList&& other) noexcept = delete;
  AttrList& operator=(AttrList const& other) = delete;
  virtual ~AttrList();

 private:
  std::vector<FileRecord> file_record_list_;
};  // AttrList

}  // namespace NtfsBrowser