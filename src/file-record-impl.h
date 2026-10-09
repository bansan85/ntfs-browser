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
#include "record/header.h"

namespace NtfsBrowser {
namespace Efs {

struct WrappedFek;

}  // namespace Efs

template <Strategy S>
class NtfsVolume;
class IndexEntry;
class IndexEntryView;

// Everything FileRecord<S> keeps out of its public header: the members, and the
// private methods that work on them. Friends of FileRecord<S> (AttrList,
// NtfsVolume) reach it through FileRecord<S>::impl_.
template <Strategy S>
class FileRecord<S>::Impl {
 public:
  Impl(FileRecord<S>& self, const NtfsVolume<S>& volume) noexcept;

  // The FileRecord this belongs to. Attributes are built over it, and private
  // methods that need a public one go through it. A pointer, not a reference:
  // FileRecord's move constructor MUST repoint it.
  FileRecord<S>* self;
  const NtfsVolume<S>* volume;
  std::unique_ptr<Record::HeaderImpl<S>> file_record;
  std::optional<ULONGLONG> file_reference;
  std::array<AttrRawCallback, Attr::attr_nums> attr_raw_call_back{};
  Mask attr_mask{Mask::All};

  // The extension records $ATTRIBUTE_LIST opened. An attribute imported from
  // one keeps a reference into its bytes, so they MUST outlive attr_list_:
  // declared first, destroyed last, and cleared after the attributes.
  // Unlike std::vector, appending never moves existing elements' addresses.
  std::list<FileRecord<S>> extension_records;
  // Aligned copies of the attributes that sit at a misaligned address in
  // record_buffer_. A parsed attribute keeps a reference into its copy, so
  // these MUST outlive attr_list_: declared before it, cleared after it.
  std::vector<std::vector<BYTE>> realigned_attrs;
  std::array<std::vector<std::unique_ptr<AttrBase<S>>>, Attr::attr_nums>
      attr_list{};

  // False makes AllocAttr() wrap $ATTRIBUTE_LIST generically, not via AttrList.
  bool resolve_attr_list{true};

  // True bypasses the deleted-record gate in ParseAttrs(): set by
  // NtfsVolume on its own internal FileRecords ($Volume, $MFT, $MFT
  // extension records), so a freed one doesn't take the whole volume down,
  // whatever include_deleted says.
  bool bypass_deleted_gate{false};

  // Owned per-instance so this FileRecord's raw bytes (viewed by NoCache
  // attributes as plain pointers/spans, no copy) are never aliased by
  // another FileRecord's read (eg. NtfsVolume's $MFT record vs. this one).
  std::vector<BYTE> record_buffer;

  void ClearAttrs() noexcept;
  [[nodiscard]] const AttrHeaderCommon&
      AlignedAttrHeader(std::span<const BYTE> bytes);
  void MergeAttributeContinuations();
  [[nodiscard]] bool AttachEfsContext();
  [[nodiscard]] std::vector<Efs::WrappedFek> ReadEfsEntries() const;
  void UserCallBack(DWORD att_type, const AttrHeaderCommon& ahc, bool& discard);
  template <typename Resident>
  [[nodiscard]] std::unique_ptr<AttrBase<S>>
      AllocAttr(const AttrHeaderCommon& ahc, bool& unhandled,
                std::unordered_set<ULONGLONG>& attr_list_chain);
  [[nodiscard]] bool ParseAttr(const AttrHeaderCommon& ahc,
                               std::unordered_set<ULONGLONG>& attr_list_chain);
  [[nodiscard]] bool ParseAttrs(std::unordered_set<ULONGLONG>& attr_list_chain);
  static void MergeStreamChain(std::vector<std::unique_ptr<AttrBase<S>>>& attrs,
                               std::vector<size_t>& indices,
                               std::vector<size_t>& to_erase);
  [[nodiscard]] static const std::vector<IndexEntryView>*
      FileNameIndexRootEntries(const AttrBase<S>& attr);
  [[nodiscard]] bool VisitAttr(std::span<const BYTE> cur,
                               const AttrHeaderCommon& head,
                               std::unordered_set<ULONGLONG>& attr_list_chain);
  [[nodiscard]] std::unique_ptr<Record::HeaderImpl<S>>
      ReadFileRecord(ULONGLONG file_ref);
  [[nodiscard]] std::optional<IndexEntry>
      VisitIndexBlock(ULONGLONG vcn, std::wstring_view file_name,
                      std::unordered_set<ULONGLONG>& visited_vcns,
                      size_t depth) const;
  [[nodiscard]] std::optional<IndexEntry>
      FindSubEntryInOrder(std::wstring_view file_name) const;
  void TraverseSubNode(ULONGLONG vcn, const SubentryCallback& se_call_back,
                       void* context,
                       std::unordered_set<ULONGLONG>& visited_vcns,
                       size_t depth) const;
  void ScanOrphanedIndexBlocks(
      const SubentryCallback& se_call_back, void* context,
      std::unordered_set<ULONGLONG>& visited_vcns) const;
};

}  // namespace NtfsBrowser
