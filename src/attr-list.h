#pragma once

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/strategy.h>

#include <ntfs-browser/data/file-record-header.h>

namespace NtfsBrowser
{
template <typename TYPE_RESIDENT, Strategy S>
class AttrList : public TYPE_RESIDENT
{
 public:
  AttrList(const AttrHeaderCommon& ahc, FileRecord<S>& fr);
  AttrList(AttrList&& other) noexcept = delete;
  AttrList(AttrList const& other) = delete;
  AttrList& operator=(AttrList&& other) noexcept = delete;
  AttrList& operator=(AttrList const& other) = delete;
  ~AttrList() override;

 private:
  std::vector<FileRecord<S>> file_record_list_;
};  // AttrList

}  // namespace NtfsBrowser