#pragma once

#include <unordered_set>

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/win-types.h>

#include <ntfs-browser/data/file-record-header.h>

namespace NtfsBrowser
{
template <typename TYPE_RESIDENT, Strategy S>
class AttrList : public TYPE_RESIDENT
{
 public:
  // attrListChain: the set of (record reference, attribute type) pairs
  // already resolved along the current $ATTRIBUTE_LIST resolution chain -
  // owned by whichever FileRecord::ParseAttrs() call started this chain,
  // and threaded down through every extension record opened along the way
  // (see FileRecord's private ParseAttrs(std::unordered_set<ULONGLONG>&)
  // overload). Used to recognize a (record, attribute type) pair already
  // resolved earlier in this same chain - self-reference, a cycle among
  // extension records, or a duplicate entry - and skip it instead of
  // resolving it again, which would recurse without bound.
  AttrList(const AttrHeaderCommon& ahc, FileRecord<S>& fr,
           std::unordered_set<ULONGLONG>& attrListChain);
  AttrList(AttrList&& other) noexcept = delete;
  AttrList(AttrList const& other) = delete;
  AttrList& operator=(AttrList&& other) noexcept = delete;
  AttrList& operator=(AttrList const& other) = delete;
  ~AttrList() override;

 private:
  std::vector<FileRecord<S>> file_record_list_;
};  // AttrList

}  // namespace NtfsBrowser