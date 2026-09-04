#pragma once

#include <list>
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
  // std::list, not std::vector: every extension record's attributes are
  // merged out into fr.attr_list_ as unique_ptr<AttrBase<S>>, but
  // AttrBase::attr_header_/AttrNonResident::attr_header_nr_ inside them are
  // references bound directly into the owning FileRecord's own inline
  // record buffer (FileRecordHeaderImpl<Strategy::FULL_CACHE>::data_, a
  // by-value 1024-byte union - see include/ntfs-browser/data/file-record-header.h).
  // A std::vector would relocate every FileRecord already constructed - and
  // therefore every attribute already merged out of one - each time a later
  // emplace_back() reallocates, leaving those references dangling (N3,
  // docs/bug-reports/2026-09-03-full-repo.md). std::list never moves
  // existing elements on insertion, so their addresses - and everything
  // pointing/referencing into them - stay valid for this object's whole
  // lifetime. Only ever appended to (emplace_back) and read via back(), so
  // std::list's lack of random access costs nothing here.
  std::list<FileRecord<S>> file_record_list_;
};  // AttrList

}  // namespace NtfsBrowser