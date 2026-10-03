#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <ntfs-browser/data/attr-defines.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/strategy.h>

#include "attr-slot.h"
#include "data/file-record-header.h"

namespace NtfsBrowser
{
namespace Efs
{
struct WrappedFek;
}  // namespace Efs

template <Strategy S>
class NtfsVolume;
class IndexEntry;

// Everything FileRecord<S> keeps out of its public header: the members, and the
// private methods that work on them. Friends of FileRecord<S> (AttrList,
// NtfsVolume) reach it through FileRecord<S>::impl_.
template <Strategy S>
class FileRecord<S>::Impl
{
 public:
  Impl(FileRecord<S>& self, const NtfsVolume<S>& volume) noexcept;

  // The FileRecord this belongs to. Attributes are built over it, and private
  // methods that need a public one go through it. A pointer, not a reference:
  // FileRecord's move constructor MUST repoint it.
  FileRecord<S>* self_;
  const NtfsVolume<S>& volume_;
  std::unique_ptr<FileRecordHeaderImpl<S>> file_record_;
  std::optional<ULONGLONG> file_reference_;
  std::array<AttrRawCallback, kAttrNums> attr_raw_call_back_{};
  Mask attr_mask_{Mask::ALL};

  // The extension records $ATTRIBUTE_LIST opened. An attribute imported from
  // one keeps a reference into its bytes, so they MUST outlive attr_list_:
  // declared first, destroyed last, and cleared after the attributes.
  // Unlike std::vector, appending never moves existing elements' addresses.
  std::list<FileRecord<S>> extension_records_;
  // Aligned copies of the attributes that sit at a misaligned address in
  // record_buffer_. A parsed attribute keeps a reference into its copy, so
  // these MUST outlive attr_list_: declared before it, cleared after it.
  std::vector<std::unique_ptr<BYTE[]>> realigned_attrs_;
  std::array<std::vector<std::unique_ptr<AttrBase<S>>>, kAttrNums> attr_list_{};

  // False makes AllocAttr() wrap $ATTRIBUTE_LIST generically, not via AttrList.
  bool resolve_attr_list_{true};

  // True bypasses the deleted-record gate in ParseAttrs(): set by
  // NtfsVolume on its own internal FileRecords ($Volume, $MFT, $MFT
  // extension records), so a freed one doesn't take the whole volume down,
  // whatever include_deleted says.
  bool bypass_deleted_gate_{false};

  // Owned per-instance so this FileRecord's raw bytes (viewed by NO_CACHE
  // attributes as plain pointers/spans, no copy) are never aliased by
  // another FileRecord's read (eg. NtfsVolume's $MFT record vs. this one).
  std::vector<BYTE> record_buffer_;

  void ClearAttrs() noexcept;
  [[nodiscard]] const AttrHeaderCommon&
      AlignedAttrHeader(std::span<const BYTE> bytes);
  void MergeAttributeContinuations();
  [[nodiscard]] bool AttachEfsContext();
  [[nodiscard]] std::vector<Efs::WrappedFek> ReadEfsEntries() const;
  void UserCallBack(DWORD attType, const AttrHeaderCommon& ahc, bool& bDiscard);
  template <typename RESIDENT>
  [[nodiscard]] std::unique_ptr<AttrBase<S>>
      AllocAttr(const AttrHeaderCommon& ahc, bool& bUnhandled,
                std::unordered_set<ULONGLONG>& attrListChain);
  [[nodiscard]] bool ParseAttr(const AttrHeaderCommon& ahc,
                               std::unordered_set<ULONGLONG>& attrListChain);
  [[nodiscard]] bool ParseAttrs(std::unordered_set<ULONGLONG>& attrListChain);
  static void MergeStreamChain(std::vector<std::unique_ptr<AttrBase<S>>>& attrs,
                               std::vector<size_t>& indices,
                               std::vector<size_t>& toErase);
  [[nodiscard]] static const std::vector<IndexEntry>*
      FileNameIndexRootEntries(const AttrBase<S>& attr);
  [[nodiscard]] bool VisitAttr(std::span<const BYTE> cur,
                               const AttrHeaderCommon& head,
                               std::unordered_set<ULONGLONG>& attrListChain);
  [[nodiscard]] std::unique_ptr<FileRecordHeaderImpl<S>>
      ReadFileRecord(ULONGLONG fileRef);
  [[nodiscard]] std::optional<IndexEntry>
      VisitIndexBlock(ULONGLONG vcn, std::wstring_view fileName,
                      std::unordered_set<ULONGLONG>& visitedVcns,
                      size_t depth) const;
  [[nodiscard]] std::optional<IndexEntry>
      FindSubEntryInOrder(std::wstring_view fileName) const;
  void TraverseSubNode(ULONGLONG vcn, SUBENTRY_CALLBACK seCallBack,
                       void* context,
                       std::unordered_set<ULONGLONG>& visitedVcns,
                       size_t depth) const;
  void
      ScanOrphanedIndexBlocks(SUBENTRY_CALLBACK seCallBack, void* context,
                              std::unordered_set<ULONGLONG>& visitedVcns) const;
};

}  // namespace NtfsBrowser
