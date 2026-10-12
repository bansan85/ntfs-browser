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

#include <ntfs-browser/attr/defines.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>

#include "attr/slot.h"
#include "data/file-record-header.h"
#include "record/header.h"
#include "upcase/upcase.h"

namespace NtfsBrowser {
namespace Efs {

struct WrappedFek;

}  // namespace Efs

namespace Attr {

template <Cache::Strategy S>
class AttrIndexAlloc;

}  // namespace Attr

namespace Io {

// How the recovery scan walks one $INDEX_ALLOCATION attribute.
struct OrphanScanPlan {
  // VCN step from one index block to the next.
  ULONGLONG clusters_per_block;
  // Blocks the attribute declares.
  ULONGLONG declared_blocks;
  // Blocks the scan visits.
  ULONGLONG scan_limit;
  // True when the scan visits fewer blocks than declared.
  bool capped;
};

// One name lookup down a B+ tree: the name, the volume's collation, and the
// index block VCNs the walk has already visited.
struct IndexSearch {
  std::wstring_view file_name;
  const UpCase::Table& upcase;
  std::unordered_set<ULONGLONG> visited_vcns;
};

// How the walk over a record's attribute headers ended.
enum class WalkEnd : BYTE {
  EndMarker,  // The terminating Attr::Type::All marker was reached.
  RanOut,     // The record ended without a marker, or an attribute overran it.
  Aborted     // An attribute was malformed, or the visit stopped the walk.
};

// Everything FileRecord<S> keeps out of its public header: the members, and the
// private methods that work on them. Friends of FileRecord<S> (AttrList,
// NtfsVolume) reach it through FileRecord<S>::impl_.
template <Cache::Strategy S>
class FileRecord<S>::Impl {
 public:
  Impl(Io::FileRecord<S>& self, const NtfsVolume<S>& volume) noexcept;

  // The FileRecord this belongs to. Attributes are built over it, and private
  // methods that need a public one go through it. A pointer, not a reference:
  // FileRecord's move constructor MUST repoint it.
  Io::FileRecord<S>* self;
  const NtfsVolume<S>* volume;
  std::unique_ptr<Record::HeaderImpl<S>> file_record;
  std::optional<ULONGLONG> file_reference;
  std::array<Attr::RawCallback, Attr::attr_nums> attr_raw_call_back{};
  Attr::Mask attr_mask{Attr::Mask::All};

  // The extension records $ATTRIBUTE_LIST opened. An attribute imported from
  // one keeps a reference into its bytes, so they MUST outlive attr_list_:
  // declared first, destroyed last, and cleared after the attributes.
  // Unlike std::vector, appending never moves existing elements' addresses.
  std::list<Io::FileRecord<S>> extension_records;
  // Aligned copies of the attributes that sit at a misaligned address in
  // record_buffer_. A parsed attribute keeps a reference into its copy, so
  // these MUST outlive attr_list_: declared before it, cleared after it.
  std::vector<std::vector<BYTE>> realigned_attrs;
  std::array<std::vector<std::unique_ptr<Attr::AttrBase<S>>>, Attr::attr_nums>
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
  [[nodiscard]] const Attr::HeaderCommon&
      AlignedAttrHeader(std::span<const BYTE> bytes);
  void MergeAttributeContinuations();
  [[nodiscard]] bool AttachEfsContext();
  [[nodiscard]] std::vector<Efs::WrappedFek> ReadEfsEntries() const;
  void UserCallBack(DWORD att_type, const Attr::HeaderCommon& ahc,
                    bool& discard);
  template <typename Resident>
  [[nodiscard]] std::unique_ptr<Attr::AttrBase<S>>
      AllocAttr(const Attr::HeaderCommon& ahc, bool& unhandled,
                std::unordered_set<ULONGLONG>& attr_list_chain);
  [[nodiscard]] bool ParseAttr(const Attr::HeaderCommon& ahc,
                               std::unordered_set<ULONGLONG>& attr_list_chain);
  [[nodiscard]] bool ParseAttrs(std::unordered_set<ULONGLONG>& attr_list_chain);
  [[nodiscard]] const Attr::HeaderCommon* FirstAttrToWalk();
  [[nodiscard]] WalkEnd
      WalkAttrHeaders(const Attr::HeaderCommon& first,
                      std::unordered_set<ULONGLONG>& attr_list_chain);
  static void
      MergeStreamChain(std::vector<std::unique_ptr<Attr::AttrBase<S>>>& attrs,
                       std::vector<size_t>& indices,
                       std::vector<size_t>& to_erase);
  [[nodiscard]] static const std::vector<IndexEntryView>*
      FileNameIndexRootEntries(const Attr::AttrBase<S>& attr);
  [[nodiscard]] bool VisitAttr(std::span<const BYTE> cur,
                               const Attr::HeaderCommon& head,
                               std::unordered_set<ULONGLONG>& attr_list_chain);
  [[nodiscard]] std::unique_ptr<Record::HeaderImpl<S>>
      ReadFileRecord(ULONGLONG file_ref);
  [[nodiscard]] std::optional<IndexEntry>
      VisitIndexBlock(ULONGLONG vcn, IndexSearch& search, size_t depth) const;
  [[nodiscard]] std::optional<IndexEntry>
      SearchIndexEntries(const std::vector<IndexEntryView>& entries,
                         IndexSearch& search, size_t child_depth,
                         bool at_root) const;
  [[nodiscard]] std::optional<IndexEntry>
      FindSubEntryInOrder(std::wstring_view file_name) const;
  void TraverseSubNode(ULONGLONG vcn, const SubentryCallback& se_call_back,
                       void* context,
                       std::unordered_set<ULONGLONG>& visited_vcns,
                       size_t depth) const;
  void ScanOrphanedIndexBlocks(
      const SubentryCallback& se_call_back, void* context,
      std::unordered_set<ULONGLONG>& visited_vcns) const;
  [[nodiscard]] OrphanScanPlan
      PlanOrphanScan(const Attr::AttrIndexAlloc<S>& alloc) const;
  void ReportOrphanedBlock(Attr::AttrIndexAlloc<S>& alloc, ULONGLONG vcn,
                           const SubentryCallback& se_call_back,
                           void* context) const;
};

}  // namespace Io

}  // namespace NtfsBrowser
