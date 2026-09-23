#pragma once

#include <ntfs-browser/win-types.h>

#include <list>
#include <unordered_set>

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/data/file-record-header.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/strategy.h>

namespace NtfsBrowser
{
template <typename TYPE_RESIDENT, Strategy S>
class AttrList : public TYPE_RESIDENT
{
 public:
  // attrListChain: (record, attribute type) pairs already resolved along
  // the current $ATTRIBUTE_LIST chain, threaded through every extension
  // record opened along the way.
  AttrList(const AttrHeaderCommon& ahc, FileRecord<S>& fr,
           std::unordered_set<ULONGLONG>& attrListChain);
  AttrList(AttrList&& other) noexcept = delete;
  AttrList(AttrList const& other) = delete;
  AttrList& operator=(AttrList&& other) noexcept = delete;
  AttrList& operator=(AttrList const& other) = delete;
  ~AttrList() override;

 private:
  // Unlike std::vector, appending never moves existing elements' addresses.
  std::list<FileRecord<S>> file_record_list_;
};  // AttrList

}  // namespace NtfsBrowser
